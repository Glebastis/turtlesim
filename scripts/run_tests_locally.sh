#!/bin/bash
set -e

echo "🔍 Running local tests..."

# Clean previous logs
rm -rf logs/*
mkdir -p logs

# Build test image
echo "📦 Building test image..."
docker build -f Dockerfile.test -t turtles_app:test .

# Run unit tests
echo "🧪 Running unit tests..."
docker run --rm turtles_app:test

# Run integration tests
echo "🔄 Running integration tests..."
docker-compose -f docker-compose.test.yml up --abort-on-container-exit --force-recreate

# Check metrics
echo "📊 Checking Prometheus metrics..."
sleep 5
curl -s http://localhost:9091/metrics | head -20

echo "✅ All tests passed!"