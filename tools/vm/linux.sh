#!/usr/bin/env bash
# strata's Linux gate VM: Ubuntu 24.04 under QEMU with HVF acceleration.
#   tools/vm/linux.sh create BASE_IMG   create the disk and cloud-init seed from a verified noble cloud image
#   tools/vm/linux.sh start | stop | ssh [CMD...] | status | sync CHECKOUT...
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
dir="$root/.scratch/linux-vm"
port="${STRATA_VM_SSH_PORT:-18772}"
cpus="${STRATA_VM_CPUS:-8}"
mem="${STRATA_VM_MEM:-16384}"
disk_size="${STRATA_VM_DISK:-120G}"
noble_sha256="6a81c37564db9b1ee84e141922625e1d7c5b389b99bb3c572e0243607d5bb4d2"

ssh_vm() {
	ssh -i "$dir/ssh_key" -p "$port" -o UserKnownHostsFile="$dir/known_hosts" \
		-o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 strata@127.0.0.1 "$@"
}

case "${1:-}" in
create)
	base="${2:?usage: linux.sh create BASE_IMG}"
	[[ "$(shasum -a 256 "$base" | cut -c1-64)" == "$noble_sha256" ]] || { echo "base image checksum mismatch" >&2; exit 1; }
	mkdir -p "$dir/seed"
	cp "$base" "$dir/base.img"
	qemu-img create -q -f qcow2 -F qcow2 -b "$dir/base.img" "$dir/disk.qcow2" "$disk_size"
	[[ -f "$dir/ssh_key" ]] || ssh-keygen -q -t ed25519 -N "" -C strata-vm -f "$dir/ssh_key"
	cat > "$dir/seed/user-data" <<-EOF
	#cloud-config
	users:
	  - name: strata
	    sudo: ALL=(ALL) NOPASSWD:ALL
	    shell: /bin/bash
	    ssh_authorized_keys:
	      - $(cat "$dir/ssh_key.pub")
	growpart: {mode: auto, devices: ['/']}
	package_update: true
	packages: [g++, clang, lld, cmake, ninja-build, git, python3, python3-venv, liburing-dev,
	           openjdk-21-jdk-headless, maven, linux-tools-generic, linux-tools-virtual,
	           zlib1g-dev, libzstd-dev, liblz4-dev, uuid-dev, curl, pkg-config]
	EOF
	printf 'instance-id: strata-linux-1\nlocal-hostname: strata-linux\n' > "$dir/seed/meta-data"
	rm -f "$dir/seed.iso"
	hdiutil makehybrid -quiet -iso -joliet -default-volume-name cidata -o "$dir/seed.iso" "$dir/seed"
	echo "created $dir"
	;;
start)
	qemu-system-x86_64 -accel hvf -cpu host -smp "$cpus" -m "$mem" \
		-drive file="$dir/disk.qcow2",if=virtio,cache=none \
		-drive file="$dir/seed.iso",media=cdrom,readonly=on \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:"$port"-:22 -device virtio-net-pci,netdev=net0 \
		-display none -serial file:"$dir/console.log" -monitor none \
		-daemonize -pidfile "$dir/qemu.pid"
	for _ in $(seq 120); do ssh_vm true 2>/dev/null && { echo "up on port $port"; exit 0; }; sleep 5; done
	echo "VM did not answer ssh; see $dir/console.log" >&2; exit 1
	;;
stop)
	ssh_vm sudo poweroff 2>/dev/null || true
	;;
status)
	[[ -f "$dir/qemu.pid" ]] && kill -0 "$(cat "$dir/qemu.pid")" 2>/dev/null && echo running || echo stopped
	;;
ssh)
	shift; ssh_vm "$@"
	;;
sync)
	# Copies each checkout's tracked and untracked (not ignored) files to ~/libs/<name>.
	shift
	for src in "$@"; do
		name=$(basename "$src")
		(cd "$src" && git ls-files -co --exclude-standard -z | xargs -0 tar -cf - 2>/dev/null) | ssh_vm "rm -rf ~/libs/$name && mkdir -p ~/libs/$name && tar -xf - -C ~/libs/$name 2>/dev/null"
	done
	;;
*)
	sed -n '2,4p' "$0"; exit 2
	;;
esac
