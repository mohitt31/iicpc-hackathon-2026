# ============================================================================
# IICPC HFT Benchmarking Platform — benchmark node pool (Terraform skeleton)
#
# Provisions the bare-metal / dedicated node pool the bot-fleet DaemonSet
# schedules onto. The point of this pool is latency isolation: pinned cores,
# isolcpus / nohz_full kernel args, and a label/taint so only benchmark
# workloads land here.
#
# STATUS: deliverable skeleton, cloud-agnostic shape. Fill in your provider
#         block and the managed-node-group resource for your platform
#         (EKS/GKE/AKS or a bare-metal provisioner). Mirrors Interface
#         Contract §10. Not applied in this repo.
# ============================================================================

terraform {
  required_version = ">= 1.5"
  # required_providers { aws = { source = "hashicorp/aws", version = "~> 5.0" } }
}

variable "cluster_name"   { type = string, default = "hft-bench" }
variable "bench_node_count" { type = number, default = 2 }
variable "bench_instance_type" {
  type        = string
  # bare-metal / metal instances so CPU pinning + isolcpus are real, not virtual
  default     = "c6i.metal"
  description = "Use a metal instance type so core isolation is genuine."
}

# Kernel command-line args applied to benchmark nodes via the node pool's
# launch template / startup script. isolcpus removes cores from the scheduler;
# nohz_full stops the timer tick on them; rcu_nocbs offloads RCU callbacks.
locals {
  bench_kernel_cmdline = "isolcpus=1-15 nohz_full=1-15 rcu_nocbs=1-15"
  bench_node_labels = {
    "node-role.hft/benchmark" = "true"
  }
  bench_node_taints = [{
    key    = "dedicated"
    value  = "benchmark"
    effect = "NO_SCHEDULE"
  }]
}

# ---- EXAMPLE: managed node group (pseudocode — adapt to your provider) -----
# resource "aws_eks_node_group" "benchmark" {
#   cluster_name    = var.cluster_name
#   node_group_name = "benchmark"
#   instance_types  = [var.bench_instance_type]
#   scaling_config { desired_size = var.bench_node_count, min_size = var.bench_node_count, max_size = var.bench_node_count }
#   labels = local.bench_node_labels
#   dynamic "taint" { for_each = local.bench_node_taints; content {
#       key = taint.value.key; value = taint.value.value; effect = taint.value.effect } }
#   launch_template { id = aws_launch_template.benchmark.id, version = "$Latest" }
# }
#
# resource "aws_launch_template" "benchmark" {
#   # user_data installs the kubelet with --cpu-manager-policy=static and applies
#   # local.bench_kernel_cmdline to GRUB, then reboots before joining the cluster.
#   user_data = base64encode(templatefile("${path.module}/bench_userdata.sh.tpl", {
#     kernel_cmdline = local.bench_kernel_cmdline
#   }))
# }

output "benchmark_pool" {
  value = {
    nodes         = var.bench_node_count
    instance_type = var.bench_instance_type
    kernel        = local.bench_kernel_cmdline
    labels        = local.bench_node_labels
    note          = "kubelet must run --cpu-manager-policy=static for Guaranteed-QoS exclusive cores"
  }
}
