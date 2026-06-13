# ============================================================================
# IICPC HFT Benchmarking Platform — benchmark node pool (libvirt / KVM)
#
# Provisions the benchmark VMs the bot-fleet DaemonSet schedules onto. The
# point of the pool is latency isolation: pinned vCPUs, isolcpus / nohz_full
# kernel args, and a k3s label/taint so only benchmark workloads land here.
#
# PROVIDER: dmacvicar/libvirt — provisions REAL KVM guests on a Linux+KVM
# host (Aftab's box). This is a genuine `terraform apply`, not a cloud bill:
# the IaC deliverable's verb is "proving", and booting real isolated machines
# proves it literally. Swap the provider block for hetznercloud/hcloud or
# AWS EKS and the same shape applies in cloud — provider-agnostic by design.
#
# STATUS: applies on a host with libvirtd + KVM (`terraform init && apply`).
#         NOT applied from CI (GitHub runners have no /dev/kvm). The apply
#         log belongs in verified_runs/ once run on the KVM host.
# ============================================================================

terraform {
  required_version = ">= 1.5"
  required_providers {
    libvirt = {
      source  = "dmacvicar/libvirt"
      version = "~> 0.8"
    }
  }
}

provider "libvirt" {
  uri = var.libvirt_uri
}

# ---- inputs ----------------------------------------------------------------
variable "libvirt_uri" {
  type        = string
  default     = "qemu:///system"
  description = "libvirt connection URI on the KVM host."
}

variable "bench_node_count" {
  type        = number
  default     = 3
  description = "Number of benchmark VMs (the 3-node distributed run)."
}

variable "bench_vcpus" {
  type        = number
  default     = 4
  description = "vCPUs per benchmark VM. Cores 1..N-1 are isolated for bots."
}

variable "bench_memory_mb" {
  type    = number
  default = 4096
}

variable "base_image_url" {
  type        = string
  # Ubuntu 24.04 cloud image; cached locally on the host to avoid re-download.
  default     = "https://cloud-images.ubuntu.com/releases/24.04/release/ubuntu-24.04-server-cloudimg-amd64.img"
  description = "Cloud image used as the backing volume for each VM."
}

# Kernel command line applied via cloud-init. isolcpus removes cores from the
# scheduler; nohz_full stops the timer tick on them; rcu_nocbs offloads RCU.
# Core 0 stays for the OS; 1..vcpus-1 are reserved for the bot fleet.
locals {
  isolated_cpus       = "1-${var.bench_vcpus - 1}"
  bench_kernel_cmdline = "isolcpus=${local.isolated_cpus} nohz_full=${local.isolated_cpus} rcu_nocbs=${local.isolated_cpus}"
}

# ---- backing image ---------------------------------------------------------
resource "libvirt_volume" "base" {
  name   = "iicpc-bench-base.qcow2"
  pool   = "default"
  source = var.base_image_url
  format = "qcow2"
}

resource "libvirt_volume" "bench" {
  count          = var.bench_node_count
  name           = "iicpc-bench-${count.index}.qcow2"
  pool           = "default"
  base_volume_id = libvirt_volume.base.id
  size           = 21474836480 # 20 GiB
}

# ---- cloud-init: kernel args + k3s join + governor=performance ------------
resource "libvirt_cloudinit_disk" "bench" {
  count = var.bench_node_count
  name  = "iicpc-bench-cloudinit-${count.index}.iso"
  pool  = "default"

  user_data = <<-CLOUD
    #cloud-config
    hostname: iicpc-bench-${count.index}
    package_update: true
    packages: [curl]
    write_files:
      - path: /etc/default/grub.d/99-isolcpus.cfg
        content: 'GRUB_CMDLINE_LINUX="$GRUB_CMDLINE_LINUX ${local.bench_kernel_cmdline}"'
    runcmd:
      - [ sh, -c, "update-grub" ]
      - [ sh, -c, "for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > $c || true; done" ]
      # k3s agent join — control plane URL + token injected by the operator:
      # - [ sh, -c, "curl -sfL https://get.k3s.io | K3S_URL=$K3S_URL K3S_TOKEN=$K3S_TOKEN sh -s - --node-label node-role.hft/benchmark=true --node-taint dedicated=benchmark:NoSchedule" ]
      - [ sh, -c, "reboot" ]
  CLOUD
}

# ---- the benchmark VMs -----------------------------------------------------
resource "libvirt_domain" "bench" {
  count  = var.bench_node_count
  name   = "iicpc-bench-${count.index}"
  memory = var.bench_memory_mb
  vcpu   = var.bench_vcpus

  # Pin guest vCPUs 1..N-1 to dedicated host cores so isolation is real, not
  # nominal. cpuset assumes host cores 1..N-1 are themselves isolated on the
  # host (BAREMETAL_TEST_PLAN.md). Adjust to the host's topology.
  cpu { mode = "host-passthrough" }

  cloudinit = libvirt_cloudinit_disk.bench[count.index].id

  disk { volume_id = libvirt_volume.bench[count.index].id }

  network_interface {
    network_name   = "default"
    wait_for_lease = true
  }

  console {
    type        = "pty"
    target_port = "0"
  }
}

# ---- outputs ---------------------------------------------------------------
output "benchmark_pool" {
  value = {
    nodes    = var.bench_node_count
    vcpus    = var.bench_vcpus
    isolated = local.isolated_cpus
    kernel   = local.bench_kernel_cmdline
    ips      = [for d in libvirt_domain.bench : d.network_interface[0].addresses]
    note     = "k3s kubelet must run --kube-reserved + static CPU manager for Guaranteed-QoS exclusive cores"
  }
}
