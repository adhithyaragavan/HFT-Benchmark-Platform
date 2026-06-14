terraform {
  required_providers {
    google = {
      source  = "hashicorp/google"
      version = "~> 5.0"
    }
    google-beta = {
      source  = "hashicorp/google-beta"
      version = "~> 5.0"
    }
  }
}

provider "google" {
  project      = var.project_id
  region       = var.region
  zone         = var.zone
  access_token = var.access_token
}

provider "google-beta" {
  project      = var.project_id
  region       = var.region
  zone         = var.zone
  access_token = var.access_token
}

# --- APIs ---
resource "google_project_service" "compute" {
  project            = var.project_id
  service            = "compute.googleapis.com"
  disable_on_destroy = false
}

resource "google_project_service" "container" {
  project            = var.project_id
  service            = "container.googleapis.com"
  disable_on_destroy = false
}

# --- VPC Network ---
resource "google_compute_network" "vpc" {
  name                    = "iicpc-vpc"
  auto_create_subnetworks = false
  depends_on              = [google_project_service.compute]
}

# --- Subnet ---
resource "google_compute_subnetwork" "subnet" {
  name          = "iicpc-subnet"
  region        = var.region
  network       = google_compute_network.vpc.name
  ip_cidr_range = "10.0.0.0/16"
  
  private_ip_google_access = true

  secondary_ip_range {
    range_name    = "k8s-pod-range"
    ip_cidr_range = "10.1.0.0/16"
  }

  secondary_ip_range {
    range_name    = "k8s-svc-range"
    ip_cidr_range = "10.2.0.0/20"
  }
}

# --- Cloud Router & NAT (for private nodes to pull images/internet access) ---
resource "google_compute_router" "router" {
  name    = "iicpc-router"
  region  = var.region
  network = google_compute_network.vpc.id
}

resource "google_compute_router_nat" "nat" {
  name                               = "iicpc-nat"
  router                             = google_compute_router.router.name
  region                             = google_compute_router.router.region
  nat_ip_allocate_option             = "AUTO_ONLY"
  source_subnetwork_ip_ranges_to_nat = "ALL_SUBNETWORKS_ALL_IP_RANGES"
}

# --- GKE Cluster ---
resource "google_container_cluster" "primary" {
  name     = var.cluster_name
  location = var.region
  
  depends_on = [google_project_service.container]

  deletion_protection = false

  # We can't create a cluster with no node pool defined, but we want to only use
  # separately managed node pools. So we create the smallest possible default
  # node pool and immediately delete it.
  remove_default_node_pool = true
  initial_node_count       = 1

  node_config {
    disk_size_gb = 30
    disk_type    = "pd-standard"
  }

  network    = google_compute_network.vpc.name
  subnetwork = google_compute_subnetwork.subnet.name

  # Enable Datapath Provider for advanced network policy
  datapath_provider = "ADVANCED_DATAPATH"



  ip_allocation_policy {
    cluster_secondary_range_name  = "k8s-pod-range"
    services_secondary_range_name = "k8s-svc-range"
  }

  private_cluster_config {
    enable_private_nodes    = true
    enable_private_endpoint = false
    master_ipv4_cidr_block  = "172.16.0.0/28"
  }

  workload_identity_config {
    workload_pool = "${var.project_id}.svc.id.goog"
  }
}

# --- GKE Node Pool ---
resource "google_container_node_pool" "primary_nodes" {
  provider   = google-beta
  name       = "primary-node-pool"
  location   = var.region
  cluster    = google_container_cluster.primary.name
  node_count = var.min_nodes

  autoscaling {
    min_node_count = var.min_nodes
    max_node_count = var.max_nodes
  }

  management {
    auto_repair  = true
    auto_upgrade = true
  }

  node_config {
    machine_type = var.machine_type
    disk_size_gb = 50
    disk_type    = "pd-standard"

    # Enable gVisor on the nodes
    sandbox_config {
      type = "GVISOR"
    }

    # Google recommends custom service accounts that have cloud-platform scope and permissions granted via IAM Roles.
    oauth_scopes = [
      "https://www.googleapis.com/auth/cloud-platform"
    ]

    labels = {
      env = "production"
    }

    tags = ["gke-node"]
  }
}

# --- Firewall Rules ---
resource "google_compute_firewall" "allow_internal" {
  name    = "iicpc-allow-internal"
  network = google_compute_network.vpc.name

  allow {
    protocol = "tcp"
  }
  allow {
    protocol = "udp"
  }
  allow {
    protocol = "icmp"
  }

  source_ranges = ["10.0.0.0/16", "10.1.0.0/16", "10.2.0.0/20"]
}
