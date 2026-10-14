#!/usr/bin/env bash
# strata's FreeBSD VM: FreeBSD 14.4 (UFS) under QEMU with HVF acceleration.
#   tools/vm/freebsd.sh create IMAGE.qcow2.xz   create the disk and cloud-init seed from a verified image
#   tools/vm/freebsd.sh start | stop | ssh [CMD...] | status | sync CHECKOUT...
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
dir="$root/.scratch/freebsd-vm"
port="${STRATA_BSD_SSH_PORT:-18774}"
cpus="${STRATA_BSD_CPUS:-4}"
mem="${STRATA_BSD_MEM:-4096}"
disk_size="${STRATA_BSD_DISK:-40G}"
image_sha256="d957fe56fb596280428328e78e9e4feae00dd361701caa79a061f9bd289eff67"  # 14.4-RELEASE amd64 BASIC-CLOUDINIT ufs

ssh_vm() {
	ssh -i "$dir/ssh_key" -p "$port" -o UserKnownHostsFile="$dir/known_hosts" \
		-o StrictHostKeyChecking=accept-new -o ConnectTimeout=5 root@127.0.0.1 "$@"
}

case "${1:-}" in
create)
	image="${2:?usage: freebsd.sh create IMAGE.qcow2.xz}"
	[[ "$(shasum -a 256 "$image" | cut -c1-64)" == "$image_sha256" ]] || { echo "image checksum mismatch" >&2; exit 1; }
	mkdir -p "$dir/seed"
	xz -dc "$image" > "$dir/base.img"
	qemu-img create -q -f qcow2 -F qcow2 -b "$dir/base.img" "$dir/disk.qcow2" "$disk_size"
	[[ -f "$dir/ssh_key" ]] || ssh-keygen -q -t ed25519 -N "" -C strata-bsd -f "$dir/ssh_key"
	cat > "$dir/seed/user-data" <<-EOF
	#cloud-config
	hostname: strata-freebsd
	ssh_pwauth: false
	ssh_authorized_keys:
	  - $(cat "$dir/ssh_key.pub")
	runcmd:
	  - "mkdir -p /root/.ssh && cp /home/freebsd/.ssh/authorized_keys /root/.ssh/ && chmod 700 /root/.ssh && chmod 600 /root/.ssh/authorized_keys && sed -i '' 's/.*PermitRootLogin.*/PermitRootLogin prohibit-password/' /etc/ssh/sshd_config && service sshd restart"
	EOF
	printf 'instance-id: strata-freebsd-2\nlocal-hostname: strata-freebsd\n' > "$dir/seed/meta-data"
	rm -f "$dir/seed.iso"
	hdiutil makehybrid -quiet -iso -joliet -default-volume-name cidata -o "$dir/seed.iso" "$dir/seed"
	echo "created $dir"
	;;
start)
	qemu-system-x86_64 -accel hvf -cpu host -smp "$cpus" -m "$mem" \
		-drive file="$dir/disk.qcow2",if=virtio,cache=none,discard=unmap,detect-zeroes=unmap \
		-drive file="$dir/seed.iso",media=cdrom,readonly=on \
		-netdev user,id=net0,hostfwd=tcp:127.0.0.1:"$port"-:22 -device virtio-net-pci,netdev=net0 \
		-display none -serial file:"$dir/console.log" -monitor none \
		-daemonize -pidfile "$dir/qemu.pid"
	for _ in $(seq 120); do ssh_vm true 2>/dev/null && { echo "up on port $port"; exit 0; }; sleep 5; done
	echo "VM did not answer ssh; see $dir/console.log" >&2; exit 1
	;;
stop)
	ssh_vm poweroff 2>/dev/null || true
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
		(cd "$src" && git ls-files -co --exclude-standard -z | xargs -0 tar -cf - 2>/dev/null) | ssh_vm "rm -rf ~/libs/$name && mkdir -p ~/libs/$name && tar -xmf - -C ~/libs/$name 2>/dev/null"
	done
	;;
*)
	sed -n '2,4p' "$0"; exit 2
	;;
esac
