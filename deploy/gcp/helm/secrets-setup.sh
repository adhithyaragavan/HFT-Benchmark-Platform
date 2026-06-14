#!/bin/bash
set -e

echo "🔑 Setting up Kubernetes Secrets for Production..."

# Database Credentials
echo "Creating DB credentials secret..."
kubectl create secret generic iicpc-db-credentials \
  --from-literal=postgres-password=$(openssl rand -base64 24) \
  --from-literal=redis-password=$(openssl rand -base64 24) \
  --dry-run=client -o yaml | kubectl apply -f -

# Kafka Credentials
echo "Creating Kafka/Redpanda credentials secret..."
kubectl create secret generic iicpc-kafka-credentials \
  --from-literal=username="platform_user" \
  --from-literal=password=$(openssl rand -base64 24) \
  --dry-run=client -o yaml | kubectl apply -f -

# Object Storage (GCS/MinIO) Credentials
# Replace these with your actual GCP Service Account HMAC keys if using GCS
echo "Creating Object Storage credentials secret..."
kubectl create secret generic iicpc-storage-credentials \
  --from-literal=access-key="PLEASE_REPLACE_ME_ACCESS_KEY" \
  --from-literal=secret-key="PLEASE_REPLACE_ME_SECRET_KEY" \
  --dry-run=client -o yaml | kubectl apply -f -

# Image Pull Secrets for GCP Artifact Registry
# Make sure you have authenticated your local gcloud before running this
echo "Creating Artifact Registry Pull Secret..."
kubectl create secret docker-registry gcr-registry-secret \
  --docker-server=us-central1-docker.pkg.dev \
  --docker-username=oauth2accesstoken \
  --docker-password=$(gcloud auth print-access-token) \
  --dry-run=client -o yaml | kubectl apply -f -

echo "✅ All secrets successfully provisioned!"
