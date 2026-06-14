#!/bin/bash
set -e

# ==============================================================================
# Cold-Start Test Script for IICPC 2026 Distributed Benchmarking Platform
# ==============================================================================
# This script automates the complete lifecycle:
# 1. Zipping a reference engine
# 2. Uploading the submission
# 3. Polling the status transition (pending -> compiling -> running)
# 4. Triggering the orchestrator phase sequence
# 5. Asserting the leaderboard UI telemetry stream
# ==============================================================================

GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

API_URL="http://localhost:8080"
TEAM_ID="omega-force"
EMAIL="omega@force.com"
LANG="cpp"

echo -e "${BLUE}[1/4] Preparing Reference Match Engine Archive...${NC}"
rm -f /tmp/submission.zip
# Zipping up the template cpp-exchange folder
cd templates/cpp-exchange && zip -r /tmp/submission.zip ./* > /dev/null
cd ../../
echo -e "${GREEN}✓ Archive created at /tmp/submission.zip${NC}\n"

echo -e "${BLUE}[2/4] Triggering First Upload for team '${TEAM_ID}'...${NC}"
# Uploading to submission-svc via API Gateway
UPLOAD_RESP=$(curl -s -X POST "${API_URL}/api/submit" \
  -F "team_id=${TEAM_ID}" \
  -F "email=${EMAIL}" \
  -F "language=${LANG}" \
  -F "file=@/tmp/submission.zip")

SUB_ID=$(echo $UPLOAD_RESP | jq -r '.submission_id')
if [ "$SUB_ID" == "null" ] || [ -z "$SUB_ID" ]; then
    echo -e "Upload Failed: $UPLOAD_RESP"
    exit 1
fi

echo -e "${GREEN}✓ Upload Successful! Submission ID: ${SUB_ID}${NC}\n"

echo -e "${BLUE}[3/4] Polling Submission State Transitions...${NC}"
CURRENT_STATE=""
while true; do
    STATUS_RESP=$(curl -s "${API_URL}/api/v1/submissions/${SUB_ID}")
    NEW_STATE=$(echo $STATUS_RESP | jq -r '.status')
    
    if [ "$NEW_STATE" != "$CURRENT_STATE" ]; then
        echo -e "  Transitioned to: ${YELLOW}${NEW_STATE}${NC}"
        CURRENT_STATE=$NEW_STATE
    fi

    if [ "$CURRENT_STATE" == "running" ]; then
        echo -e "${GREEN}✓ Container is LIVE and RUNNING!${NC}\n"
        break
    elif [ "$CURRENT_STATE" == "failed" ]; then
        echo -e "Submission failed to compile/run."
        exit 1
    fi
    sleep 0.5
done

echo -e "${BLUE}[4/4] Kickstarting Bot Orchestrator Traffic Phases...${NC}"
# Starting the benchmark run for the team
START_RESP=$(curl -s -X POST "${API_URL}/api/v1/teams/${TEAM_ID}/start")
echo -e "${GREEN}✓ Orchestrator start hook triggered: ${START_RESP}${NC}\n"

# Polling the Phase Tracker
echo -e "${BLUE}[5/5] Monitoring Active Leaderboard UI Stream...${NC}"
echo -e "Checking Orchestrator Phase and Redis Telemetry (Ctrl+C to exit)\n"

for i in {1..20}; do
    PHASE_RESP=$(curl -s "${API_URL}/api/v1/orchestrator/status/${TEAM_ID}")
    PHASE=$(echo $PHASE_RESP | jq -r '.current_phase')
    
    # Grabbing real-time leaderboard data
    LB_RESP=$(curl -s "${API_URL}/api/v1/leaderboard")
    ENTRY=$(echo $LB_RESP | jq -c ".[] | select(.team_id == \"${TEAM_ID}\")")
    
    if [ -z "$ENTRY" ] || [ "$ENTRY" == "null" ]; then
        echo -e "[Phase: ${YELLOW}${PHASE:-idle}${NC}] Leaderboard: Waiting for first telemetry payload..."
    else
        TPS=$(echo $ENTRY | jq -r '.max_tps')
        LATENCY=$(echo $ENTRY | jq -r '.latency_p99_us')
        CORRECT=$(echo $ENTRY | jq -r '.correctness')
        SCORE=$(echo $ENTRY | jq -r '.composite_score')
        
        echo -e "[Phase: ${YELLOW}${PHASE}${NC}] Leaderboard LIVE 🚀 | TPS: ${TPS} | P99 Latency: ${LATENCY}µs | Correctness: ${CORRECT} | Score: ${SCORE}"
    fi
    sleep 1
done

echo -e "\n${GREEN}✓ Cold Start Lifecycle Complete!${NC}"
