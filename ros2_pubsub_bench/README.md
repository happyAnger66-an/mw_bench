# ros2_pubsub_bench

一个简单的 C++ ROS 2 pub/sub 压测节点，支持通过**命令行**指定：

- **topic name**：`--topic /xxx`
- **topic size(bytes)**（仅 pub 生效）：`--size 1048576`
- **topic hz**（仅 pub 生效）：`--hz 100`

## 构建

先确保已 `source` ROS 2 环境（含 `ament_cmake`、`rclcpp`），再在工作空间根目录构建：

```bash
source /opt/ros/<distro>/setup.bash
colcon build --packages-select ros2_pubsub_bench
source install/setup.bash
```

## 使用方法

### Publisher

```bash
ros2 run ros2_pubsub_bench pubsub_bench -- \
  --mode pub \
  --topic /chatter \
  --size 1048576 \
  --hz 100
```

### Subscriber

```bash
ros2 run ros2_pubsub_bench pubsub_bench -- \
  --mode sub \
  --topic /chatter
```

## 输出说明

- **pub 模式**：只打印 pub 侧吞吐 `msg/s` 与 `Mb/s`
- **sub 模式**：只打印 sub 侧吞吐，并打印 `latency avg`（毫秒）

## 延迟计算（跨主机）

延迟 `dt` 使用 **wall clock（系统时间）** 计算：`dt = now(system_clock) - send_time(system_clock)`。

跨主机测试时需要两台机器的系统时间同步（如 **NTP/Chrony**），否则 `dt` 可能出现明显偏差或为负（程序会忽略负值样本）。

