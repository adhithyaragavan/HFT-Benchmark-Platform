# ============================================================================
# IICPC 2026 — Distributed Benchmarking Platform
# Terraform Root Module — Infrastructure Provisioning
# ============================================================================

terraform {
  required_version = ">= 1.5"
  required_providers {
    google = {
      source  = "hashicorp/google"
      version = "~> 5.0"
    }
    kubernetes = {
      source  = "hashicorp/kubernetes"
      version = "~> 2.25"
    }
    helm = {
      source  = "hashicorp/helm"
      version = "~> 2.12"
    }
  }
}

variable "project_id" {
  type        = string
  description = "GCP project ID"
}

variable "region" {
  type    = string
  default = "us-central1"
}

variable "cluster_name" {
  type    = string
  default = "iicpc-benchmark-cluster"
}

variable "node_count" {
  type    = number
  default = 3
}

variable "machine_type" {
  type    = string
  default = "e2-standard-8"
}

# ─── GKE Cluster ─────────────────────────────────────────────────────

resource "google_container_cluster" "primary" {
  name     = var.cluster_name
  location = var.region

  # Autopilot mode for easier management during hackathon
  enable_autopilot = true

  network    = google_compute_network.vpc.name
  subnetwork = google_compute_subnetwork.subnet.name

  # Enable gVisor (Sandbox) for contestant workloads
  node_pool_defaults {
    node_config_defaults {
      gcfs_config {
        enabled = true
      }
    }
  }

  release_channel {
    channel = "RAPID"
  }

  deletion_protection = false
}

# ─── VPC Network ─────────────────────────────────────────────────────

resource "google_compute_network" "vpc" {
  name                    = "${var.cluster_name}-vpc"
  auto_create_subnetworks = false
}

resource "google_compute_subnetwork" "subnet" {
  name          = "${var.cluster_name}-subnet"
  ip_cidr_range = "10.10.0.0/16"
  region        = var.region
  network       = google_compute_network.vpc.id

  secondary_ip_range {
    range_name    = "pods"
    ip_cidr_range = "10.20.0.0/14"
  }

  secondary_ip_range {
    range_name    = "services"
    ip_cidr_range = "10.24.0.0/20"
  }
}

# ─── Cloud SQL (PostgreSQL for metadata) ─────────────────────────────

resource "google_sql_database_instance" "postgres" {
  name             = "${var.cluster_name}-postgres"
  database_version = "POSTGRES_16"
  region           = var.region

  settings {
    tier = "db-custom-2-8192"

    ip_configuration {
      ipv4_enabled    = false
      private_network = google_compute_network.vpc.id
    }

    backup_configuration {
      enabled = true
    }
  }

  deletion_protection = false
}

resource "google_sql_database" "benchmarks" {
  name     = "benchmarks"
  instance = google_sql_database_instance.postgres.name
}

# ─── Memorystore (Redis for leaderboard) ─────────────────────────────

resource "google_redis_instance" "leaderboard" {
  name           = "${var.cluster_name}-redis"
  tier           = "BASIC"
  memory_size_gb = 1
  region         = var.region

  authorized_network = google_compute_network.vpc.id

  redis_configs = {
    maxmemory-policy = "allkeys-lru"
  }
}

# ─── Artifact Registry (container images) ────────────────────────────

resource "google_artifact_registry_repository" "platform" {
  location      = var.region
  repository_id = "platform-images"
  format        = "DOCKER"
}

resource "google_artifact_registry_repository" "contestants" {
  location      = var.region
  repository_id = "contestant-images"
  format        = "DOCKER"
}

# ─── Outputs ─────────────────────────────────────────────────────────

output "cluster_endpoint" {
  value = google_container_cluster.primary.endpoint
}

output "redis_host" {
  value = google_redis_instance.leaderboard.host
}

output "postgres_connection" {
  value     = google_sql_database_instance.postgres.connection_name
  sensitive = true
}

output "registry_url" {
  value = "${var.region}-docker.pkg.dev/${var.project_id}/platform-images"
}
