#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# One-shot installer for st2110-vcard.
#
# Two modes:
#   simple  -- MTL's kernel data path. No IOMMU, no vfio, no VF binding, no
#              patched NIC driver. Works on any NIC with an IP. Gives up
#              ST 2110-21 narrow pacing, so: bring-up and 1080p tests, not a
#              facility.
#   full    -- DPDK PMD on an E810 VF. Everything the real thing needs, and
#              everything that has to be set up for it.
#
# Re-running is safe; each step checks whether it is already done.

set -euo pipefail

MODE=""
IFACE=""
PF_BDF=""
MTL_DIR="${MTL_DIR:-$(cd "$(dirname "$0")/../Media-Transport-Library" 2>/dev/null && pwd || true)}"
CONFIG=/etc/st2110-vcard.json
HUGEPAGES=${HUGEPAGES:-2048}
ASSUME_YES=0

RED=$'\e[31m'; GRN=$'\e[32m'; YLW=$'\e[33m'; BLD=$'\e[1m'; RST=$'\e[0m'
say()  { echo "${BLD}==>${RST} $*"; }
ok()   { echo "  ${GRN}ok${RST}   $*"; }
warn() { echo "  ${YLW}warn${RST} $*"; }
die()  { echo "  ${RED}fail${RST} $*" >&2; exit 1; }

usage() {
	cat <<EOF
usage: $0 --simple  --iface <ifname>   [options]
       $0 --full    --pf <bb:dd.f>     [options]

  --simple            kernel data path, minimal setup (bring-up / 1080p)
  --full              DPDK PMD on an E810 VF (full ST 2110-21 pacing)
  --iface NAME        network interface for --simple, e.g. enp3s0f0
  --pf BDF            PF PCI address for --full, e.g. 0000:4b:00.0
  --mtl-dir PATH      Media-Transport-Library checkout (default: ../Media-Transport-Library)
  --config PATH       config file to write (default: $CONFIG)
  --hugepages N       2M hugepages to reserve (default: $HUGEPAGES)
  -y                  do not ask before installing packages

Run without arguments to see what the machine already has.
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
	--simple) MODE=simple ;;
	--full) MODE=full ;;
	--iface) IFACE="$2"; shift ;;
	--pf) PF_BDF="$2"; shift ;;
	--mtl-dir) MTL_DIR="$2"; shift ;;
	--config) CONFIG="$2"; shift ;;
	--hugepages) HUGEPAGES="$2"; shift ;;
	-y) ASSUME_YES=1 ;;
	-h|--help) usage; exit 0 ;;
	*) usage; exit 1 ;;
	esac
	shift
done

SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO=sudo

# ---------------------------------------------------------------- diagnosis
say "Checking what this machine already has"

have_mtl=0
if pkg-config --exists mtl 2>/dev/null; then
	ok "MTL installed (version $(pkg-config --modversion mtl))"
	have_mtl=1
else
	warn "MTL not installed"
fi

have_ffmpeg=0
if pkg-config --atleast-version=59.37 libavcodec 2>/dev/null; then
	ok "FFmpeg dev headers (libavcodec $(pkg-config --modversion libavcodec))"
	have_ffmpeg=1
else
	warn "FFmpeg dev headers missing or older than 5.1"
fi

hp=$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo 0)
[ "$hp" -gt 0 ] && ok "hugepages: $hp" || warn "no hugepages reserved"

iommu=0
if [ -d /sys/kernel/iommu_groups ] && [ -n "$(ls -A /sys/kernel/iommu_groups 2>/dev/null)" ]; then
	ok "IOMMU enabled"; iommu=1
else
	warn "IOMMU not enabled (only needed for --full)"
fi

if modinfo ice 2>/dev/null | grep -qi "^version:.*Kahawai"; then
	ok "patched ice driver: $(modinfo ice | awk '/^version:/{print $2}')"
elif modinfo ice >/dev/null 2>&1; then
	warn "stock ice driver -- no MTL rate limit patches (only needed for --full)"
fi

if [ -z "$MODE" ]; then
	echo
	echo "Nothing changed. Pick a mode:"
	echo "  $0 --simple --iface <ifname>    # minimal, no reboot"
	echo "  $0 --full   --pf <bb:dd.f>      # full pacing, needs IOMMU + reboot"
	exit 0
fi

[ -n "$MTL_DIR" ] && [ -d "$MTL_DIR" ] || die "Media-Transport-Library not found; pass --mtl-dir"

# ---------------------------------------------------------------- packages
if [ "$have_ffmpeg" -eq 0 ] || [ "$have_mtl" -eq 0 ]; then
	say "Installing build dependencies"
	if [ "$ASSUME_YES" -ne 1 ]; then
		read -rp "  install apt packages? [Y/n] " a
		case "${a:-y}" in [nN]*) die "aborted" ;; esac
	fi
	$SUDO apt-get update -qq
	$SUDO apt-get install -y -qq git gcc g++ cmake meson python3 python3-pip \
		pkg-config libnuma-dev libjson-c-dev libpcap-dev libssl-dev \
		systemtap-sdt-dev llvm clang flex byacc linuxptp \
		libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
		libswresample-dev
	$SUDO pip install -q --break-system-packages pyelftools ninja 2>/dev/null || \
		$SUDO pip install -q pyelftools ninja
	ok "packages installed"
fi

# ---------------------------------------------------------------- full-mode prerequisites
if [ "$MODE" = full ]; then
	[ -n "$PF_BDF" ] || die "--full needs --pf <bb:dd.f>"
	if [ "$iommu" -eq 0 ]; then
		cat <<EOF

${RED}IOMMU is off, and --full cannot work without it.${RST}
Enable VT-d in the BIOS, then:

  ${SUDO} sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT="/GRUB_CMDLINE_LINUX_DEFAULT="intel_iommu=on iommu=pt /' /etc/default/grub
  ${SUDO} update-grub && ${SUDO} reboot

Then run this script again. (Or use --simple, which needs none of this.)
EOF
		exit 1
	fi
	if ! modinfo ice 2>/dev/null | grep -qi "^version:.*Kahawai"; then
		say "Building the ice driver with MTL's rate limit patches"
		$SUDO "$MTL_DIR/script/build_ice_driver.sh"
		ok "ice driver rebuilt -- repeat this after every kernel update"
	fi
fi

# ---------------------------------------------------------------- DPDK + MTL
if [ "$have_mtl" -eq 0 ]; then
	say "Building DPDK and MTL (this takes a while)"
	$SUDO "$MTL_DIR/script/build_dpdk.sh"
	( cd "$MTL_DIR" && ./build.sh )
	$SUDO ldconfig
	pkg-config --exists mtl || die "MTL build finished but mtl.pc is not on PKG_CONFIG_PATH"
	ok "MTL $(pkg-config --modversion mtl)"
fi

# ---------------------------------------------------------------- hugepages
say "Reserving hugepages"
$SUDO sysctl -q -w vm.nr_hugepages="$HUGEPAGES"
echo "vm.nr_hugepages=$HUGEPAGES" | $SUDO tee /etc/sysctl.d/99-st2110-vcard.conf >/dev/null
ok "$HUGEPAGES pages, persisted across reboots"

# ---------------------------------------------------------------- port setup
if [ "$MODE" = full ]; then
	say "Creating a VF on $PF_BDF and binding it to vfio-pci"
	$SUDO modprobe vfio-pci
	$SUDO "$MTL_DIR/script/nicctl.sh" create_vf "$PF_BDF"
	VF_BDF=$("$MTL_DIR/script/nicctl.sh" list "$PF_BDF" 2>/dev/null |
		 grep -oE '[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-9a-f]' | head -1)
	[ -n "$VF_BDF" ] || die "could not determine the VF address; check nicctl.sh list $PF_BDF"
	PORT="$VF_BDF"
	ok "VF $VF_BDF"
	LOCAL_IP="192.168.96.10"
	warn "set device.local_ip in $CONFIG to this host's address in the media VLAN"
else
	[ -n "$IFACE" ] || die "--simple needs --iface <ifname>"
	ip link show "$IFACE" >/dev/null 2>&1 || die "no such interface: $IFACE"
	LOCAL_IP=$(ip -4 -o addr show "$IFACE" | awk '{print $4}' | cut -d/ -f1 | head -1)
	[ -n "$LOCAL_IP" ] || die "$IFACE has no IPv4 address -- give it one first"
	PORT="kernel:$IFACE"
	ok "$IFACE ($LOCAL_IP), kernel data path"
	# On this path the kernel still owns the NIC, so PTP is ptp4l's job and
	# MTL reads the NIC clock it disciplines. MTL's own PTP client is meant
	# for the DPDK path.
	PHC=$(ethtool -T "$IFACE" 2>/dev/null | awk '/PTP Hardware Clock:/{print $4}')
	if [ -n "$PHC" ] && [ "$PHC" != none ] && [ -e "/dev/ptp$PHC" ]; then
		PHC_DEV="/dev/ptp$PHC"
		if pgrep -x ptp4l >/dev/null 2>&1; then
			ok "PTP: ptp4l is running, using $PHC_DEV"
		else
			warn "PTP: using $PHC_DEV, but ptp4l is not running -- start it:  ${SUDO} ptp4l -i $IFACE -m -s"
		fi
	else
		PHC_DEV=""
		warn "PTP: $IFACE has no hardware clock; timing will be free-running"
	fi
fi

# ---------------------------------------------------------------- build
say "Building st2110-vcard"
cd "$(dirname "$0")"
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
cmake --build build -j"$(nproc)"
$SUDO cmake --install build >/dev/null
ok "installed to /usr/local/bin/st2110-vcard"

# ---------------------------------------------------------------- config
$SUDO mkdir -p /var/lib/st2110-vcard/media /var/lib/st2110-vcard/recordings

# The interface that reaches the default gateway is the management side; the
# GUI and NMOS bind there, never on the media port.
MGMT_IP=$(ip -4 route get 1.1.1.1 2>/dev/null | grep -oP 'src \K\S+' | head -1)
: "${MGMT_IP:=$LOCAL_IP}"

if [ -f "$CONFIG" ]; then
	warn "$CONFIG exists, leaving it alone"
else
	say "Writing $CONFIG"
	PTP_BUILTIN=true
	[ "$MODE" = simple ] && PTP_BUILTIN=false
	$SUDO sed -e "s|\"port\": \"0000:4b:01.0\"|\"port\": \"$PORT\"|" \
		 -e "s|\"ptp\": true|\"ptp\": $PTP_BUILTIN|" \
		 -e "s|\"phc_device\": \"\"|\"phc_device\": \"${PHC_DEV:-}\"|" \
		 -e "s|\"local_ip\": \"192.168.96.10\"|\"local_ip\": \"$LOCAL_IP\"|" \
		 -e "s|\"host_address\": \"10.0.0.20\"|\"host_address\": \"$MGMT_IP\"|" \
		 config/vcard.example.json | $SUDO tee "$CONFIG" >/dev/null
	ok "written"
	warn "check before going live: multicast groups, and nmos.registry_address"
fi

# ---------------------------------------------------------------- done
cat <<EOF

${GRN}Done.${RST} Start it in the foreground first so you can read the log:

  ${SUDO} /usr/local/bin/st2110-vcard -c $CONFIG -v

GUI: http://$MGMT_IP:8088
Media files go in /var/lib/st2110-vcard/media

Once it behaves, run it as a service:

  ${SUDO} cp packaging/st2110-vcard.service /etc/systemd/system/
  ${SUDO} systemctl enable --now st2110-vcard
EOF
