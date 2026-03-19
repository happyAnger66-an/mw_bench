#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/byte_multi_array.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using ByteMultiArray = std_msgs::msg::ByteMultiArray;
constexpr size_t kHeaderSize = 16;  // 8 bytes seq + 8 bytes send_time_ns

inline int64_t steady_now_ns()
{
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

inline void write_u64(uint8_t * dst, uint64_t v)
{
  std::memcpy(dst, &v, sizeof(v));
}

inline void write_i64(uint8_t * dst, int64_t v)
{
  std::memcpy(dst, &v, sizeof(v));
}

inline int64_t read_i64(const uint8_t * src)
{
  int64_t v{};
  std::memcpy(&v, src, sizeof(v));
  return v;
}

inline rclcpp::QoS make_qos(int depth, const std::string & reliability)
{
  auto qos = rclcpp::QoS(rclcpp::KeepLast(std::max(1, depth)));
  if (reliability == "best_effort") {
    qos.best_effort();
  } else {
    qos.reliable();
  }
  return qos;
}
}  // namespace

class PubSubBenchNode final : public rclcpp::Node
{
public:
  struct Config
  {
    std::string mode{"pub"};               // pub|sub
    std::string topic{"/bench"};           // topic name
    int size_bytes{1024};                  // payload size in bytes (pub)
    double hz{100.0};                      // publish rate (pub)
    int qos_depth{10};
    std::string reliability{"reliable"};   // reliable|best_effort
    int log_period_ms{1000};
  };

  explicit PubSubBenchNode(Config cfg)
  : rclcpp::Node("pubsub_bench"),
    cfg_(std::move(cfg))
  {
    normalize_and_validate();
    qos_ = make_qos(cfg_.qos_depth, cfg_.reliability);

    if (cfg_.mode == "pub") {
      setup_publisher();
    } else if (cfg_.mode == "sub") {
      setup_subscriber();
    } else {
      RCLCPP_FATAL(get_logger(), "Invalid mode '%s' (expected pub|sub)", cfg_.mode.c_str());
      throw std::runtime_error("invalid mode");
    }

    stats_timer_ = create_wall_timer(
      std::chrono::milliseconds(cfg_.log_period_ms),
      std::bind(&PubSubBenchNode::log_stats, this));

    RCLCPP_INFO(
      get_logger(),
      "pubsub_bench started: mode=%s topic=%s size_bytes=%d hz=%.3f qos_depth=%d reliability=%s",
      cfg_.mode.c_str(), cfg_.topic.c_str(), cfg_.size_bytes, cfg_.hz, cfg_.qos_depth, cfg_.reliability.c_str());
  }

private:
  void normalize_and_validate()
  {
    if (cfg_.topic.empty() || cfg_.topic[0] != '/') {
      cfg_.topic = "/" + cfg_.topic;
    }
    if (cfg_.size_bytes < 0) {
      cfg_.size_bytes = 0;
    }
    if (cfg_.hz <= 0.0) {
      cfg_.hz = 1.0;
    }
    if (cfg_.log_period_ms <= 0) {
      cfg_.log_period_ms = 1000;
    }
    if (!(cfg_.reliability == "reliable" || cfg_.reliability == "best_effort")) {
      cfg_.reliability = "reliable";
    }
  }

  void setup_publisher()
  {
    pub_ = create_publisher<ByteMultiArray>(cfg_.topic, qos_);
    msg_template_.data.assign(static_cast<size_t>(cfg_.size_bytes), 0xAB);

    const auto period = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::duration<double>(1.0 / cfg_.hz));

    pub_timer_ = create_wall_timer(period, std::bind(&PubSubBenchNode::publish_once, this));
  }

  void setup_subscriber()
  {
    sub_ = create_subscription<ByteMultiArray>(
      cfg_.topic,
      qos_,
      [this](const ByteMultiArray::SharedPtr msg) { on_msg(*msg); });
  }

  void publish_once()
  {
    auto msg = msg_template_;
    if (msg.data.size() >= kHeaderSize) {
      write_u64(msg.data.data(), ++seq_);
      write_i64(msg.data.data() + 8, steady_now_ns());
    }

    pub_->publish(std::move(msg));
    pub_msgs_.fetch_add(1, std::memory_order_relaxed);
    pub_bytes_.fetch_add(static_cast<uint64_t>(msg_template_.data.size()), std::memory_order_relaxed);
  }

  void on_msg(const ByteMultiArray & msg)
  {
    sub_msgs_.fetch_add(1, std::memory_order_relaxed);
    sub_bytes_.fetch_add(static_cast<uint64_t>(msg.data.size()), std::memory_order_relaxed);

    if (msg.data.size() >= kHeaderSize) {
      const int64_t send_ns = read_i64(msg.data.data() + 8);
      const int64_t now_ns = steady_now_ns();
      const int64_t dt = now_ns - send_ns;
      if (dt >= 0) {
        latency_sum_ns_.fetch_add(static_cast<uint64_t>(dt), std::memory_order_relaxed);
        latency_cnt_.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  void log_stats()
  {
    const auto now = std::chrono::steady_clock::now();
    const auto dt = now - last_log_time_;
    last_log_time_ = now;

    const double sec = std::chrono::duration<double>(dt).count();
    if (sec <= 0.0) {
      return;
    }

    const uint64_t pub_msgs = pub_msgs_.exchange(0, std::memory_order_relaxed);
    const uint64_t pub_bytes = pub_bytes_.exchange(0, std::memory_order_relaxed);
    const uint64_t sub_msgs = sub_msgs_.exchange(0, std::memory_order_relaxed);
    const uint64_t sub_bytes = sub_bytes_.exchange(0, std::memory_order_relaxed);

    const uint64_t lat_cnt = latency_cnt_.exchange(0, std::memory_order_relaxed);
    const uint64_t lat_sum = latency_sum_ns_.exchange(0, std::memory_order_relaxed);

    const double pub_mps = pub_msgs / sec;
    const double pub_mbps = (pub_bytes * 8.0) / (sec * 1e6);
    const double sub_mps = sub_msgs / sec;
    const double sub_mbps = (sub_bytes * 8.0) / (sec * 1e6);

    if (lat_cnt > 0) {
      const double avg_ms = (lat_sum / static_cast<double>(lat_cnt)) / 1e6;
      RCLCPP_INFO(
        get_logger(),
        "stats(%.2fs): pub %.1f msg/s %.1f Mb/s | sub %.1f msg/s %.1f Mb/s | latency avg %.3f ms (n=%lu)",
        sec, pub_mps, pub_mbps, sub_mps, sub_mbps, avg_ms, static_cast<unsigned long>(lat_cnt));
    } else {
      RCLCPP_INFO(
        get_logger(),
        "stats(%.2fs): pub %.1f msg/s %.1f Mb/s | sub %.1f msg/s %.1f Mb/s",
        sec, pub_mps, pub_mbps, sub_mps, sub_mbps);
    }
  }

  Config cfg_;
  rclcpp::QoS qos_{10};

  rclcpp::Publisher<ByteMultiArray>::SharedPtr pub_;
  rclcpp::Subscription<ByteMultiArray>::SharedPtr sub_;
  rclcpp::TimerBase::SharedPtr pub_timer_;
  rclcpp::TimerBase::SharedPtr stats_timer_;

  ByteMultiArray msg_template_;
  uint64_t seq_{0};

  std::chrono::steady_clock::time_point last_log_time_{std::chrono::steady_clock::now()};

  std::atomic<uint64_t> pub_msgs_{0};
  std::atomic<uint64_t> pub_bytes_{0};
  std::atomic<uint64_t> sub_msgs_{0};
  std::atomic<uint64_t> sub_bytes_{0};
  std::atomic<uint64_t> latency_cnt_{0};
  std::atomic<uint64_t> latency_sum_ns_{0};
};

namespace
{
void print_usage()
{
  std::fprintf(
    stderr,
    "Usage:\n"
    "  pubsub_bench [--mode pub|sub] --topic <name> [--size <bytes>] [--hz <rate>] [--qos-depth <n>] [--reliability reliable|best_effort] [--log-ms <ms>] [--ros-args ...]\n"
    "\n"
    "Examples:\n"
    "  # publisher\n"
    "  ros2 run ros2_pubsub_bench pubsub_bench -- --mode pub --topic /chatter --size 1048576 --hz 100\n"
    "  # subscriber\n"
    "  ros2 run ros2_pubsub_bench pubsub_bench -- --mode sub --topic /chatter\n"
    "\n");
}

bool starts_with(const std::string & s, const char * prefix)
{
  return s.rfind(prefix, 0) == 0;
}

PubSubBenchNode::Config parse_cli(int argc, char ** argv)
{
  PubSubBenchNode::Config cfg;

  // Remove ROS-specific args first, so our parser sees only user args.
  const auto non_ros = rclcpp::remove_ros_arguments(argc, argv);
  std::vector<std::string> args;
  args.reserve(non_ros.size());
  for (const auto & a : non_ros) {
    args.push_back(a);
  }

  // args[0] is program name
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string & a = args[i];
    if (a == "-h" || a == "--help") {
      print_usage();
      std::exit(0);
    }

    auto need_value = [&](const char * flag) -> const std::string & {
      if (i + 1 >= args.size()) {
        throw std::runtime_error(std::string("missing value for ") + flag);
      }
      return args[++i];
    };

    if (a == "--mode") {
      cfg.mode = need_value("--mode");
    } else if (a == "--topic") {
      cfg.topic = need_value("--topic");
    } else if (a == "--size" || a == "--size-bytes") {
      cfg.size_bytes = std::stoi(need_value(a.c_str()));
    } else if (a == "--hz") {
      cfg.hz = std::stod(need_value("--hz"));
    } else if (a == "--qos-depth") {
      cfg.qos_depth = std::stoi(need_value("--qos-depth"));
    } else if (a == "--reliability") {
      cfg.reliability = need_value("--reliability");
    } else if (a == "--log-ms") {
      cfg.log_period_ms = std::stoi(need_value("--log-ms"));
    } else if (starts_with(a, "--")) {
      throw std::runtime_error("unknown flag: " + a);
    } else {
      // ignore positional args
    }
  }

  return cfg;
}
}  // namespace

int main(int argc, char ** argv)
{
  rclcpp::init(argc, argv);
  PubSubBenchNode::Config cfg;
  try {
    cfg = parse_cli(argc, argv);
  } catch (const std::exception & e) {
    std::fprintf(stderr, "Error: %s\n\n", e.what());
    print_usage();
    rclcpp::shutdown();
    return 2;
  }

  rclcpp::spin(std::make_shared<PubSubBenchNode>(std::move(cfg)));
  rclcpp::shutdown();
  return 0;
}

