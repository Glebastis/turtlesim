# turtlesim

ROS 2 project for spawning, controlling, and observing turtles in [turtlesim](https://docs.ros.org/en/humble/Tutorials/Beginner-CLI-Tools/Introducing-Turtlesim/Introducing-Turtlesim.html), with an optional Docker-based observability stack (Prometheus, Grafana, Loki).

## Packages

| Package | Description |
|---------|-------------|
| `my_cpp_pkg` | C++ nodes: `turtle_manager` and `turtle_driver` |
| `my_robot_interfaces` | Custom interfaces: `TurtleInfo` message, `RemoveTurtle` service |
| `first_launchfile_bringup` | Launch files and parameters |

## Project layout

```
my_cpp_pkg/                 # turtle_manager / turtle_driver
my_robot_interfaces/        # TurtleInfo, RemoveTurtle
first_launchfile_bringup/   # launch and params
config/ launch/             # bind-mounted into Docker
grafana/                    # dashboards and datasources
tests/ scripts/
Dockerfile* docker-compose*
Makefile run.sh stop.sh
```

## Quick start (Docker)

Requires Docker and Docker Compose. On Linux, an X11 display is needed for the turtlesim GUI.

```bash
make build
make up
make status
make down
```

### Useful Make targets

| Target | Description |
|--------|-------------|
| `make build` | Build the Docker image |
| `make up` | Start all services (`./run.sh`) |
| `make down` | Stop all services (`./stop.sh`) |
| `make logs` | Follow container logs |
| `make status` | Show service health |
| `make shell` | Open a shell in `ros2_turtles_app` |
| `make metrics` | Print turtle-related Prometheus metrics |
| `make restart` | Restart the ROS 2 app container |
| `make clean` | Remove containers, volumes, and local data |

### Observability endpoints

| Service | URL |
|---------|-----|
| Prometheus | http://localhost:9090 |
| Grafana | http://localhost:3000 (`admin` / `admin`) |
| Loki | http://localhost:3100 |
| Turtle Manager metrics | http://localhost:9091/metrics |
| Turtle Controller metrics | http://localhost:9092/metrics |

## Local build (ROS 2 Humble)

```bash
cd /path/to/ros2_ws   # workspace that contains this repo under src/
colcon build --packages-select my_robot_interfaces my_cpp_pkg first_launchfile_bringup
source install/setup.bash
ros2 launch first_launchfile_bringup turtle_sim_launch.py
```

## License

See [LICENSE](LICENSE).
