#!/bin/bash

set -e

echo "========================================="
echo "ROS2 Turtlesim with Observability Stack"
echo "========================================="

# Функция для проверки команды
check_command() {
    if ! command -v $1 &> /dev/null; then
        echo "Error: $1 is not installed"
        echo "Please install Docker first: https://docs.docker.com/engine/install/"
        exit 1
    fi
}

# Проверка зависимостей
check_command docker
check_command docker compose

# Создание необходимых директорий
mkdir -p logs
mkdir -p grafana/provisioning/datasources
mkdir -p grafana/provisioning/dashboards
mkdir -p grafana/dashboards
mkdir -p config
mkdir -p launch

# Настройка X11 для GUI (Linux)
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    # Проверяем, запущен ли X сервер
    if [[ -n "$DISPLAY" ]]; then
        xhost +local:docker 2>/dev/null || true
        echo "X11 forwarding enabled for DISPLAY=$DISPLAY"
    else
        echo "Warning: DISPLAY not set, GUI may not work"
    fi
fi

# Проверка наличия Dockerfile
if [ ! -f "Dockerfile" ]; then
    echo "Error: Dockerfile not found in current directory"
    exit 1
fi

# Сборка Docker образа
echo "Building Docker image..."
docker compose build turtles_app

# Запуск всех сервисов
echo "Starting all services..."
docker compose up -d

# Ожидание запуска
echo "Waiting for services to start..."
sleep 10

# Проверка статуса
echo ""
echo "Service Status:"
docker compose ps

# Проверка метрик
echo ""
echo "Checking metrics endpoints..."
sleep 2

# Проверка доступности метрик
if curl -s http://localhost:9091/metrics > /dev/null 2>&1; then
    echo "✓ Turtle Manager metrics available at http://localhost:9091/metrics"
else
    echo "⚠ Turtle Manager metrics not yet available (may take a few seconds)"
fi

if curl -s http://localhost:9092/metrics > /dev/null 2>&1; then
    echo "✓ Turtle Controller metrics available at http://localhost:9092/metrics"
else
    echo "⚠ Turtle Controller metrics not yet available (may take a few seconds)"
fi

echo ""
echo "========================================="
echo "Services Available at:"
echo "========================================="
echo "Prometheus:    http://localhost:9090"
echo "Grafana:       http://localhost:3000 (admin/admin)"
echo "Loki:          http://localhost:3100"
echo ""
echo "Turtle Manager Metrics: http://localhost:9091/metrics"
echo "Turtle Controller Metrics: http://localhost:9092/metrics"
echo ""
echo "Useful commands:"
echo "  View logs:          docker compose logs -f"
echo "  Stop services:      docker compose down"
echo "  Restart app:        docker compose restart turtles_app"
echo "  Shell in container: docker exec -it ros2_turtles_app /bin/bash"
echo "========================================="

# Показать последние логи
echo ""
echo "Recent application logs:"
docker compose logs --tail=20 turtles_app

echo ""
echo "Watching application logs (Ctrl+C to stop watching, services continue)..."
echo ""

# Показать логи приложения (опционально)
docker compose logs -f turtles_app