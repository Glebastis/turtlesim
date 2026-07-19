cmake_minimum_required(VERSION 3.8)
project(first_launchfile_bringup)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(ament_cmake REQUIRED)

# Install launch files directly
install(DIRECTORY
  launch
  DESTINATION share/${PROJECT_NAME}/
)

# Install config files
install(DIRECTORY
  config
  DESTINATION share/${PROJECT_NAME}/
)

# Install Python launch files as executables
install(PROGRAMS
  launch/turtle_sim_launch.py
  DESTINATION share/${PROJECT_NAME}/launch
)

ament_package()