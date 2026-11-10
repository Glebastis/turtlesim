# Используем официальный ROS2 Humble как базовый образ
FROM osrf/ros:humble-desktop-full

# Устанавливаем переменные окружения
ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC
ENV ROS_DOMAIN_ID=42
ENV RMW_IMPLEMENTATION=rmw_fastrtps_cpp

RUN sed -i 's|http://archive.ubuntu.com/ubuntu/|http://ru.archive.ubuntu.com/ubuntu/|g' /etc/apt/sources.list \
 && sed -i 's|http://security.ubuntu.com/ubuntu/|http://ru.security.ubuntu.com/ubuntu/|g' /etc/apt/sources.list
# Устанавливаем системные зависимости
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    libssl-dev \
    libcurl4-openssl-dev \
    libcivetweb-dev \
    pkg-config \
    && rm -rf /var/lib/apt/lists/*

# Устанавливаем prometheus-cpp
RUN cd /tmp && \
    git clone --depth 1 --branch v1.3.0 https://github.com/jupp0r/prometheus-cpp.git && \
    cd prometheus-cpp && \
    git submodule update --init --recursive --depth 1 && \
    mkdir -p _build && cd _build && \
    cmake .. \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_SHARED_LIBS=ON \
        -DENABLE_PUSH=OFF \
        -DENABLE_COMPRESSION=OFF \
        -DUSE_THREADS=ON && \
    make -j$(nproc) && \
    make install && \
    ldconfig && \
    cd / && rm -rf /tmp/prometheus-cpp

# Создаем рабочую директорию для ROS2 workspace
RUN mkdir -p /ros2_ws/src

# Копируем пакеты
COPY ./my_robot_interfaces /ros2_ws/src/my_robot_interfaces/
COPY ./my_cpp_pkg /ros2_ws/src/my_cpp_pkg/

# Создаем директории для логов
RUN mkdir -p /var/log/ros2 && \
    chmod 755 /var/log/ros2

# Собираем пакеты
WORKDIR /ros2_ws
RUN /bin/bash -c "source /opt/ros/humble/setup.bash && \
    colcon build --packages-select my_robot_interfaces && \
    colcon build --packages-select my_cpp_pkg --cmake-args -DCMAKE_BUILD_TYPE=Release"

# Проверяем бинарные файлы
RUN ls -la /ros2_ws/install/my_cpp_pkg/lib/my_cpp_pkg/ && \
    ldd /ros2_ws/install/my_cpp_pkg/lib/my_cpp_pkg/turtle_manager_logging | grep -E "prometheus|not found" || echo "Prometheus linking check"

# Создаем скрипт для запуска
RUN echo '#!/bin/bash\n\
set -e\n\
\n\
# Source ROS2 environment\n\
source /opt/ros/humble/setup.bash\n\
source /ros2_ws/install/setup.bash\n\
\n\
# Set up logging\n\
export RCUTILS_CONSOLE_OUTPUT_FORMAT="[{severity} {time}] [{name}]: {message}"\n\
export RCUTILS_COLORIZED_OUTPUT=1\n\
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH\n\
\n\
# Create log directory\n\
LOG_DIR="/var/log/ros2/$(date +%Y%m%d_%H%M%S)"\n\
mkdir -p $LOG_DIR\n\
\n\
echo "========================================="\n\
echo "ROS2 Turtle Manager Starting..."\n\
echo "Log directory: $LOG_DIR"\n\
echo "========================================="\n\
\n\
# Start turtlesim\n\
echo "Starting turtlesim_node..."\n\
ros2 run turtlesim turtlesim_node 2>&1 | tee -a "$LOG_DIR/turtlesim.log" &\n\
TURTLESIM_PID=$!\n\
\n\
sleep 5\n\
\n\
# Start manager\n\
echo "Starting turtle_manager..."\n\
ros2 run my_cpp_pkg turtle_manager_logging 2>&1 | tee -a "$LOG_DIR/turtle_manager.log" &\n\
MANAGER_PID=$!\n\
\n\
# Start driver\n\
echo "Starting turtle_driver..."\n\
ros2 run my_cpp_pkg turtle_driver_logging 2>&1 | tee -a "$LOG_DIR/turtle_driver.log" &\n\
DRIVER_PID=$!\n\
\n\
echo "========================================="\n\
echo "All nodes started!"\n\
echo "Turtlesim PID: $TURTLESIM_PID"\n\
echo "Manager PID: $MANAGER_PID"\n\
echo "Driver PID: $DRIVER_PID"\n\
echo "========================================="\n\
\n\
# Keep running\n\
wait $TURTLESIM_PID\n\
' > /run_all.sh && chmod +x /run_all.sh

ENTRYPOINT ["/run_all.sh"]
