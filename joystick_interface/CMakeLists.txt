cmake_minimum_required(VERSION 2.8.3)
project(joystick_interface)

find_package(catkin REQUIRED COMPONENTS
  rospy
  message_generation
  vortex_msgs
)

generate_messages(
  DEPENDENCIES
  std_msgs
)

catkin_package(CATKIN_DEPENDS rospy message_runtime vortex_msgs)
