#!/usr/bin/env bash
PROJECT_DIR="/Users/adhithyaragavan/.gemini/antigravity/scratch/trading-platform"
cd "$PROJECT_DIR"

# Terminate any existing platform service processes
pkill -f "build/services" || true
sleep 1

echo "Starting sandbox-mgr..."
nohup ./build/services/sandbox-mgr/sandbox-mgr > logs-sandbox-mgr.txt 2>&1 &

echo "Starting gateway..."
nohup ./build/services/gateway/gateway > logs-gateway.txt 2>&1 &

echo "Starting submission-svc..."
nohup ./build/services/submission-svc/submission_svc > logs-submission-svc.txt 2>&1 &

echo "Starting scoring-engine..."
nohup ./build/services/scoring-engine/scoring-engine > logs-scoring-engine.txt 2>&1 &

echo "Starting leaderboard-svc..."
nohup ./build/services/leaderboard-svc/leaderboard-svc > logs-leaderboard-svc.txt 2>&1 &

echo "Starting bot-orchestrator..."
nohup ./build/services/bot-orchestrator/bot-orchestrator > logs-bot-orchestrator.txt 2>&1 &

echo "Starting bot-worker..."
nohup ./build/services/bot-worker/bot_worker > logs-bot-worker.txt 2>&1 &

sleep 2
ps aux | grep -v grep | grep "build/services"
