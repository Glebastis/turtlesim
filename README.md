# turtlesim

Проект по управлению черепашкой в экосистеме ROS2.

## Структура

```
my_cpp_pkg/                 # turtle_manager / turtle_driver
my_robot_interfaces/        # TurtleInfo, RemoveTurtle
first_launchfile_bringup/   # launch и params
config/ launch/             # для docker volumes
grafana/                    # дашборды и datasources
tests/ scripts/
Dockerfile* docker-compose*
Makefile run.sh stop.sh
```

## Быстрый старт (Docker)

```bash
make build
make up
make status
make down
```

## Сборка локально (ROS 2 Humble)

```bash
cd /path/to/ros2_ws   # workspace, где лежит этот репозиторий в src/
colcon build --packages-select my_robot_interfaces my_cpp_pkg first_launchfile_bringup
source install/setup.bash
ros2 launch first_launchfile_bringup turtle_sim_launch.py
```
