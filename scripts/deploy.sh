#!/bin/bash
set -e

ENVIRONMENT=${1:-production}
echo "🚀 Deploying to $ENVIRONMENT..."

# Pull latest images
docker-compose pull

# Backup current logs
mkdir -p backups
tar -czf "backups/logs-$(date +%Y%m%d-%H%M%S).tar.gz" logs/

# Deploy with health check
docker-compose up -d --force-recreate --remove-orphans

# Wait for services to be healthy
echo "⏳ Waiting for services to be healthy..."
sleep 10

# Check health
docker-compose ps | grep unhealthy && echo "❌ Some services are unhealthy!" && exit 1

# Verify metrics
curl -s http://localhost:9091/metrics > /dev/null && echo "✅ Metrics endpoint OK"
curl -s http://localhost:3000 > /dev/null && echo "✅ Grafana OK"

echo "✅ Deployment successful!"