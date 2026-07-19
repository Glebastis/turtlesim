#!/bin/bash

echo "========================================="
echo "Stopping ROS2 Turtlesim Stack"
echo "========================================="

# Сохраняем логи перед остановкой
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_FILE="logs/shutdown_log_${TIMESTAMP}.log"

echo "Saving logs to $LOG_FILE..."
mkdir -p logs
docker compose logs > "$LOG_FILE" 2>&1

# Останавливаем все сервисы
echo "Stopping all services..."
docker compose down

echo ""
echo "========================================="
echo "All services stopped"
echo "Logs saved to: $LOG_FILE"
echo "========================================="

# Очистка X11 permissions (Linux)
if [[ "$OSTYPE" == "linux-gnu"* ]]; then
    xhost -local:docker 2>/dev/null || true
fi