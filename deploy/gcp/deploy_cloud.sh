#!/bin/bash
set -e

# ==============================================================================
# IICPC Platform - Cloud Deployment Automation (GCP)
# ==============================================================================

echo "🚀 Initiating Cloud Deployment for IICPC Platform..."

# Automatically retrieve and export OAuth token for Terraform (both google & google-beta) and CLI tools
export GOOGLE_OAUTH_ACCESS_TOKEN=$(gcloud auth print-access-token)
export TF_VAR_access_token=$GOOGLE_OAUTH_ACCESS_TOKEN

# 1. Provision Infrastructure via Terraform
echo "📦 Step 1: Provisioning Infrastructure via Terraform..."
cd terraform
# Re-run init to ensure the google-beta provider is downloaded and updated
terraform init -upgrade
terraform apply -auto-approve
cd ..

# Retrieve Cluster Credentials
echo "🔑 Step 2: Fetching GKE Cluster Credentials..."
CLUSTER_NAME=$(terraform -chdir=terraform output -raw cluster_name 2>/dev/null || echo "iicpc-cluster")
REGION=$(terraform -chdir=terraform output -raw region 2>/dev/null || echo "us-central1")
gcloud container clusters get-credentials $CLUSTER_NAME --region $REGION

# 2. Setup Kubernetes Secrets
echo "🛡️ Step 3: Configuring Kubernetes Secrets..."
./helm/secrets-setup.sh

# 3. Apply Strict Network Policies (Sandbox Isolation)
echo "🔒 Step 4: Applying gVisor Sandbox Network Policies..."
kubectl apply -f k8s/network-policies.yaml

# 4. Deploy the Application Stack via Helm
echo "⛵ Step 5: Deploying microservices via Helm..."
# Note: Ensure you have built and pushed your images to Artifact Registry before this step!
# Update the 'repository' fields in your Chart values to point to your GCP registry.

helm upgrade --install iicpc-platform ../helm-chart \
  -f helm/production-values.yaml \
  --namespace default \
  --wait \
  --timeout 10m

# 5. Output Access Information
echo "======================================================================"
echo "✅ Deployment Successful!"
echo ""
echo "📡 Gateway Public IP:"
kubectl get svc gateway -o jsonpath='{.status.loadBalancer.ingress[0].ip}'
echo ""
echo ""
echo "📝 Note: Ensure you create the GCP Artifact Registry repository first:"
echo "   gcloud artifacts repositories create iicpc-repo \\"
echo "     --repository-format=docker \\"
echo "     --location=$REGION \\"
echo "     --description=\"IICPC Docker Repository\""
echo "======================================================================"
