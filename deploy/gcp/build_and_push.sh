#!/bin/bash
set -e

# Configuration
PROJECT_ID="iicpc-summer-hackathn"
REGION="us-central1"
REPO_NAME="iicpc-repo"
REGISTRY="${REGION}-docker.pkg.dev/${PROJECT_ID}/${REPO_NAME}"

# Ensure script runs from the project root directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/../.."

echo "============================================================"
echo "🚀 Building and Pushing IICPC Platform to Artifact Registry"
echo "Registry: $REGISTRY"
echo "Working Directory: $(pwd)"
echo "============================================================"

# List of microservices to build and push
# Assuming each service has a Dockerfile in its respective directory
SERVICES=(
  "bot-orchestrator"
  "bot-worker"
  "gateway"
  "leaderboard-svc"
  "sandbox-mgr"
  "scoring-engine"
  "submission-svc"
)

# Authenticate docker to artifact registry
echo "🔑 Configuring Docker authentication for GCP Artifact Registry..."
gcloud auth configure-docker ${REGION}-docker.pkg.dev --quiet

for SERVICE in "${SERVICES[@]}"; do
  echo "------------------------------------------------------------"
  echo "📦 Processing $SERVICE..."
  
  IMAGE_TAG="${REGISTRY}/${SERVICE}:latest"
  
  # Build the Docker image
  echo "🔨 Building $SERVICE..."
  docker build -t "$IMAGE_TAG" -f "services/${SERVICE}/Dockerfile" .
  
  # Push to Artifact Registry
  echo "☁️ Pushing $SERVICE to GCP..."
  docker push "$IMAGE_TAG"
done

# Frontend Dashboard (if applicable)
if [ -d "services/web-dashboard" ]; then
  echo "------------------------------------------------------------"
  echo "📦 Processing web-dashboard..."
  IMAGE_TAG="${REGISTRY}/web-dashboard:latest"
  docker build -t "$IMAGE_TAG" -f "services/web-dashboard/Dockerfile" .
  docker push "$IMAGE_TAG"
fi

echo "============================================================"
echo "✅ All images successfully pushed to GCP Artifact Registry!"
echo "You can now run ./deploy/gcp/deploy_cloud.sh to deploy the platform."
echo "============================================================"
