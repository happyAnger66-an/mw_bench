cmake_minimum_required(VERSION 3.8)
project(ros2_pubsub_bench)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(std_msgs REQUIRED)

add_executable(pubsub_bench src/pubsub_bench.cpp)
ament_target_dependencies(pubsub_bench rclcpp std_msgs)
target_compile_features(pubsub_bench PUBLIC cxx_std_17)
install(TARGETS pubsub_bench DESTINATION lib/${PROJECT_NAME})

ament_package()

