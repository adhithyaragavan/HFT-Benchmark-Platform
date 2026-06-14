variable "project_id" {
  description = "The GCP Project ID"
  type        = string
  default     = "iicpc-summer-hackathn"
}

variable "region" {
  description = "The GCP Region"
  type        = string
  default     = "us-central1"
}

variable "zone" {
  description = "The GCP Zone"
  type        = string
  default     = "us-central1-a"
}

variable "cluster_name" {
  description = "The name of the GKE cluster"
  type        = string
  default     = "iicpc-cluster"
}

variable "machine_type" {
  description = "The machine type for GKE nodes. Must support gVisor (nested virtualization or specific standard types)."
  type        = string
  default     = "e2-standard-4"
}

variable "min_nodes" {
  description = "Minimum number of nodes for autoscaling"
  type        = number
  default     = 2
}

variable "max_nodes" {
  description = "Maximum number of nodes for autoscaling (capped to fit 12 vCPU project quota)"
  type        = number
  default     = 3
}

variable "access_token" {
  description = "The GCP OAuth2 access token to authenticate providers."
  type        = string
  default     = null
}
