.PHONY: help build up down logs clean shell metrics restart status

help:
	@echo "Available commands:"
	@echo "  make build    - Build Docker image"
	@echo "  make up       - Start all services"
	@echo "  make down     - Stop all services"
	@echo "  make logs     - Show logs"
	@echo "  make clean    - Clean all containers and volumes"
	@echo "  make shell    - Open shell in turtles_app container"
	@echo "  make metrics  - Show metrics from both nodes"
	@echo "  make restart  - Restart turtles_app only"
	@echo "  make status   - Show service status"

build:
	docker compose build

up:
	./run.sh

down:
	./stop.sh

logs:
	docker compose logs -f

clean:
	docker compose down -v
	docker system prune -f
	rm -rf logs/*
	rm -rf grafana_data/* 2>/dev/null || true
	rm -rf prometheus_data/* 2>/dev/null || true
	rm -rf loki_data/* 2>/dev/null || true

shell:
	docker exec -it ros2_turtles_app /bin/bash

metrics:
	@echo "=== Turtle Manager Metrics ==="
	@curl -s http://localhost:9091/metrics | grep "turtle_" | grep -v "^#" | head -15 || echo "Metrics not available"
	@echo ""
	@echo "=== Turtle Controller Metrics ==="
	@curl -s http://localhost:9092/metrics | grep "turtle_" | grep -v "^#" | head -15 || echo "Metrics not available"

restart:
	docker compose restart turtles_app
	@echo "Turtles_app restarted"

status:
	docker compose ps
	@echo ""
	@echo "=== Service Health ==="
	@curl -s http://localhost:9090/api/v1/query?query=up 2>/dev/null | grep -q "success" && echo "✓ Prometheus OK" || echo "⚠ Prometheus not responding"
	@curl -s http://localhost:3000 2>/dev/null | grep -q "Grafana" && echo "✓ Grafana OK" || echo "⚠ Grafana not responding"
	@curl -s http://localhost:3100/ready 2>/dev/null | grep -q "ready" && echo "✓ Loki OK" || echo "⚠ Loki not responding"
	@docker exec ros2_turtles_app ros2 node list 2>/dev/null | grep -q "turtle" && echo "✓ ROS2 nodes running" || echo "⚠ ROS2 nodes not running"