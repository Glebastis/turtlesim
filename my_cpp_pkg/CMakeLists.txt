cmake_minimum_required(VERSION 3.8)
project(my_cpp_pkg)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Find ROS2 dependencies
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(turtlesim REQUIRED)
find_package(geometry_msgs REQUIRED)
find_package(my_robot_interfaces REQUIRED)

# Prometheus support - явно указываем библиотеки
find_library(PROMETHEUS_PULL_LIB prometheus-cpp-pull REQUIRED)
find_library(PROMETHEUS_CORE_LIB prometheus-cpp-core REQUIRED)
find_library(CIVETWEB_LIB civetweb REQUIRED)
find_path(PROMETHEUS_INCLUDE_DIR prometheus/exposer.h REQUIRED)

if(PROMETHEUS_PULL_LIB AND PROMETHEUS_CORE_LIB AND PROMETHEUS_INCLUDE_DIR)
  message(STATUS "✅ Prometheus found at: ${PROMETHEUS_INCLUDE_DIR}")
  set(WITH_PROMETHEUS TRUE)
  set(PROMETHEUS_LINK_LIBRARIES 
    ${PROMETHEUS_PULL_LIB}
    ${PROMETHEUS_CORE_LIB}
    ${CIVETWEB_LIB}
    pthread
  )
else()
  message(FATAL_ERROR "❌ Prometheus libraries not found!")
endif()

# Turtle Manager Node
add_executable(turtle_manager_logging src/turtle_manager_logging.cpp)
target_compile_features(turtle_manager_logging PUBLIC c_std_99 cxx_std_17)

# Используем ament_target_dependencies для ROS2 пакетов (ОСНОВНОЕ ИСПРАВЛЕНИЕ!)
ament_target_dependencies(turtle_manager_logging
  rclcpp
  turtlesim
  geometry_msgs
  my_robot_interfaces
)

# Добавляем Prometheus
target_include_directories(turtle_manager_logging PRIVATE ${PROMETHEUS_INCLUDE_DIR})
target_compile_definitions(turtle_manager_logging PRIVATE WITH_PROMETHEUS)
target_link_libraries(turtle_manager_logging ${PROMETHEUS_LINK_LIBRARIES})

# Turtle Driver Node
add_executable(turtle_driver_logging src/turtle_driver_logging.cpp)
target_compile_features(turtle_driver_logging PUBLIC c_std_99 cxx_std_17)

ament_target_dependencies(turtle_driver_logging
  rclcpp
  turtlesim
  geometry_msgs
  my_robot_interfaces
)

target_include_directories(turtle_driver_logging PRIVATE ${PROMETHEUS_INCLUDE_DIR})
target_compile_definitions(turtle_driver_logging PRIVATE WITH_PROMETHEUS)
target_link_libraries(turtle_driver_logging ${PROMETHEUS_LINK_LIBRARIES})

# Install
install(TARGETS
  turtle_manager_logging
  turtle_driver_logging
  DESTINATION lib/${PROJECT_NAME}
)

ament_package()