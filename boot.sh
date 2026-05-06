#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

RELAY_BIND="${RELAY_BIND:-0.0.0.0}"
RELAY_PORT="${RELAY_PORT:-17523}"
RELAY_CMD="${RELAY_CMD:-$SCRIPT_DIR/bin/apx-relayd}"

L4T_DIR="${L4T_DIR:-${HOME}/jetson-flash/Linux_for_Tegra}"
QEMU_I386_PROXY="${QEMU_I386_PROXY:-}"
QEMU_I386_PROXY_DEFAULT="$SCRIPT_DIR/bin/qemu-i386-apxproxy"

APX_RELAY_HOST="${APX_RELAY_HOST:-host.docker.internal:$RELAY_PORT}"
APX_FAKE_USB_PATH="${APX_FAKE_USB_PATH:-/dev/bus/usb/001/001}"
FLASH_TARGET_BOARD="${FLASH_TARGET_BOARD:-p3768-0000-p3767-0000-a0-qspi}"
BOOTDEV="${BOOTDEV:-internal}"
QSPI_XML="${QSPI_XML:-bootloader/generic/cfg/flash_t234_qspi.xml}"

DOCKER_IMAGE="${DOCKER_IMAGE:-ubuntu:22.04}"
DOCKER_PLATFORM="${DOCKER_PLATFORM:-linux/amd64}"
CONTAINER_L4T="${CONTAINER_L4T:-/work/Linux_for_Tegra}"

log() {
  printf '[boot] %s\n' "$*" >&2
}

die() {
  printf '[boot] ERROR: %s\n' "$*" >&2
  exit 1
}

usage() {
  printf 'usage: ./boot.sh sd ssd | ./boot.sh ssd sd\n' >&2
}

normalize_boot_device() {
  case "${1:-}" in
    sd) printf 'sd\n' ;;
    ssd) printf 'nvme\n' ;;
    *) return 1 ;;
  esac
}

overlay_name_for() {
  case "$1" in
    sd,nvme) printf 'BootOrderLocalSdNvme.dtbo\n' ;;
    nvme,sd) printf 'BootOrderLocalNvmeSd.dtbo\n' ;;
    *) return 1 ;;
  esac
}

resolve_qemu_i386_proxy() {
  if [ -n "$QEMU_I386_PROXY" ]; then
    printf '%s\n' "$QEMU_I386_PROXY"
    return 0
  fi

  if [ -x "$QEMU_I386_PROXY_DEFAULT" ]; then
    printf '%s\n' "$QEMU_I386_PROXY_DEFAULT"
    return 0
  fi

  return 1
}

describe_qemu_i386_proxy() {
  if resolve_qemu_i386_proxy; then
    return 0
  fi

  printf '<missing: set QEMU_I386_PROXY or install %s>\n' "$QEMU_I386_PROXY_DEFAULT"
}

parse_boot_priority() {
  [ "$#" -eq 2 ] || return 1

  local first
  local second
  first="$(normalize_boot_device "$1")" || return 1
  second="$(normalize_boot_device "$2")" || return 1

  [ "$first" != "$second" ] || return 1
  printf '%s,%s\n' "$first" "$second"
}

print_dry_run() {
  local boot_priority="$1"
  local boot_order_overlay="$2"

  cat <<EOF
DefaultBootPriority=$boot_priority
ADDITIONAL_DTB_OVERLAY=$boot_order_overlay
relay: $RELAY_CMD -b $RELAY_BIND -p $RELAY_PORT
docker image: $DOCKER_IMAGE ($DOCKER_PLATFORM)
L4T_DIR=$L4T_DIR
QEMU_I386_PROXY=$(describe_qemu_i386_proxy)
container flash: USER=root ADDITIONAL_DTB_OVERLAY=$boot_order_overlay ./flash.sh --no-systemimg --qspi-only --usb-instance $APX_FAKE_USB_PATH -c $QSPI_XML $FLASH_TARGET_BOARD $BOOTDEV
EOF
}

check_host_requirements() {
  [ "$(uname -s)" = "Darwin" ] || die "this script must run on macOS with the Jetson in APX/recovery mode"
  command -v docker >/dev/null 2>&1 || die "docker is not in PATH"
  [ -x "$RELAY_CMD" ] || die "missing executable relay: $RELAY_CMD"
  [ -d "$L4T_DIR" ] || die "L4T_DIR does not exist: $L4T_DIR"
  [ -x "$L4T_DIR/flash.sh" ] || die "missing executable: $L4T_DIR/flash.sh"
  [ -x "$L4T_DIR/bootloader/tegrarcm_v2" ] || die "missing executable: $L4T_DIR/bootloader/tegrarcm_v2"
  [ -x "$L4T_DIR/bootloader/tegradevflash_v2" ] || die "missing executable: $L4T_DIR/bootloader/tegradevflash_v2"
  if ! QEMU_I386_PROXY="$(resolve_qemu_i386_proxy)"; then
    die "missing qemu APX proxy: set QEMU_I386_PROXY or install $QEMU_I386_PROXY_DEFAULT"
  fi
  [ -x "$QEMU_I386_PROXY" ] || die "qemu APX proxy is not executable: $QEMU_I386_PROXY"
}

wait_for_relay() {
  local relay_pid="$1"

  if ! kill -0 "$relay_pid" 2>/dev/null; then
    die "relay exited before Docker flash started"
  fi

  if command -v nc >/dev/null 2>&1; then
    local attempt
    for attempt in 1 2 3 4 5; do
      if nc -z 127.0.0.1 "$RELAY_PORT" >/dev/null 2>&1; then
        return 0
      fi
      sleep 1
      kill -0 "$relay_pid" 2>/dev/null || die "relay exited while waiting for port $RELAY_PORT"
    done
  else
    sleep 1
  fi
}

run_docker_flash() {
  local boot_priority="$1"
  local boot_order_overlay="$2"

  local docker_args=(
    run
    --rm
    -i
    --privileged
    --platform "$DOCKER_PLATFORM"
    --add-host=host.docker.internal:host-gateway
    -v "$L4T_DIR:$CONTAINER_L4T"
    -v "$QEMU_I386_PROXY:/opt/qemu-i386-apxproxy:ro"
    -w "$CONTAINER_L4T"
    -e "APX_RELAY_HOST=$APX_RELAY_HOST"
    -e "APX_RELAY_ADDR=$APX_RELAY_HOST"
    -e "APX_FAKE_USB_PATH=$APX_FAKE_USB_PATH"
    -e "BOOT_PRIORITY=$boot_priority"
    -e "BOOT_ORDER_OVERLAY=$boot_order_overlay"
    -e "FLASH_TARGET_BOARD=$FLASH_TARGET_BOARD"
    -e "BOOTDEV=$BOOTDEV"
    -e "QSPI_XML=$QSPI_XML"
  )

  if [ -t 1 ]; then
    docker_args+=(-t)
  fi

  docker_args+=(
    "$DOCKER_IMAGE"
    bash
    -s
  )

  log "Launching Docker flash image: $DOCKER_IMAGE on $DOCKER_PLATFORM"
  log "Relay endpoint: $APX_RELAY_HOST"
  log "DefaultBootPriority: $boot_priority"
  log "QSPI flash target: $FLASH_TARGET_BOARD $BOOTDEV"

  docker "${docker_args[@]}" <<'CONTAINER_SCRIPT'
set -euo pipefail

log() {
  printf '[container] %s\n' "$*" >&2
}

export DEBIAN_FRONTEND=noninteractive
if ! command -v dtc >/dev/null 2>&1 || ! ldconfig -p 2>/dev/null | grep -q 'libglib-2.0.so.0'; then
  log "Installing qemu/L4T runtime dependencies"
  apt-get update >/dev/null
  apt-get install -y --no-install-recommends \
    abootimg binutils bzip2 cpio cpp device-tree-compiler dosfstools file \
    iproute2 iputils-ping lbzip2 libglib2.0-0 libxml2-utils netcat-openbsd \
    openssl python3 python3-yaml rsync sudo udev uuid-runtime xxd \
    xmlstarlet zlib1g zstd lz4 >/dev/null
fi

restore_usb_tools() {
  if [ -n "${TEGRARCM_BACKUP:-}" ] && [ -f "${TEGRARCM_BACKUP}" ]; then
    cp -p "${TEGRARCM_BACKUP}" ./bootloader/tegrarcm_v2
  fi
  if [ -n "${TEGRADEVFLASH_BACKUP:-}" ] && [ -f "${TEGRADEVFLASH_BACKUP}" ]; then
    cp -p "${TEGRADEVFLASH_BACKUP}" ./bootloader/tegradevflash_v2
  fi
}

cleanup_container() {
  restore_usb_tools
  rm -f "kernel/dtb/${BOOT_ORDER_OVERLAY%.dtbo}.dts"
  rm -f "kernel/dtb/${BOOT_ORDER_OVERLAY}"
  rm -f "bootloader/${BOOT_ORDER_OVERLAY}"
}
trap cleanup_container EXIT

mkdir -p "$(dirname -- "${APX_FAKE_USB_PATH}")"
: > "${APX_FAKE_USB_PATH}"

export APX_RELAY_HOST
export APX_RELAY_ADDR="${APX_RELAY_ADDR:-$APX_RELAY_HOST}"

log "Generating ${BOOT_ORDER_OVERLAY} with DefaultBootPriority=${BOOT_PRIORITY}"
cat > "kernel/dtb/${BOOT_ORDER_OVERLAY%.dtbo}.dts" <<EOF_DTS
/dts-v1/;
/plugin/;

/ {
    overlay-name = "UEFI boot order ${BOOT_PRIORITY}";

    fragment@0 {
        target-path = "/";

        board_config {
            sw-modules = "uefi";
        };

        __overlay__ {
            firmware {
                uefi {
                    variables {
                        gNVIDIATokenSpaceGuid {
                            DefaultBootPriority {
                                data = "${BOOT_PRIORITY}";
                                locked;
                            };
                        };
                    };
                };
            };
        };
    };
};
EOF_DTS
dtc -@ -I dts -O dtb \
  -o "kernel/dtb/${BOOT_ORDER_OVERLAY}" \
  "kernel/dtb/${BOOT_ORDER_OVERLAY%.dtbo}.dts"

log "Running APX UID probe through qemu APX proxy"
set -x
/opt/qemu-i386-apxproxy ./bootloader/tegrarcm_v2 --instance "${APX_FAKE_USB_PATH}" --new_session --chip 0x23 --uid
set +x

log "Installing temporary USB tool wrappers for flash.sh"
TEGRARCM_BACKUP="$(mktemp /tmp/tegrarcm_v2.real.XXXXXX)"
cp -p ./bootloader/tegrarcm_v2 "${TEGRARCM_BACKUP}"
cat > ./bootloader/tegrarcm_v2 <<EOF_WRAPPER
#!/bin/sh
exec /opt/qemu-i386-apxproxy "${TEGRARCM_BACKUP}" "\$@"
EOF_WRAPPER
chmod +x ./bootloader/tegrarcm_v2

TEGRADEVFLASH_BACKUP="$(mktemp /tmp/tegradevflash_v2.real.XXXXXX)"
cp -p ./bootloader/tegradevflash_v2 "${TEGRADEVFLASH_BACKUP}"
cat > ./bootloader/tegradevflash_v2 <<EOF_WRAPPER
#!/bin/sh
exec /opt/qemu-i386-apxproxy "${TEGRADEVFLASH_BACKUP}" "\$@"
EOF_WRAPPER
chmod +x ./bootloader/tegradevflash_v2

log "Running QSPI-only boot-order flash"
set -x
USER=root ADDITIONAL_DTB_OVERLAY="${BOOT_ORDER_OVERLAY}" \
  ./flash.sh --no-systemimg --qspi-only \
    --usb-instance "${APX_FAKE_USB_PATH}" \
    -c "${QSPI_XML}" \
    "${FLASH_TARGET_BOARD}" \
    "${BOOTDEV}"
CONTAINER_SCRIPT
}

main() {
  local boot_priority
  if ! boot_priority="$(parse_boot_priority "$@")"; then
    usage
    exit 2
  fi

  local boot_order_overlay
  boot_order_overlay="$(overlay_name_for "$boot_priority")"

  if [ "${BOOT_SH_DRY_RUN:-0}" = "1" ]; then
    print_dry_run "$boot_priority" "$boot_order_overlay"
    return 0
  fi

  check_host_requirements

  local relay_pid=""
  cleanup() {
    if [ -n "$relay_pid" ] && kill -0 "$relay_pid" 2>/dev/null; then
      kill "$relay_pid" 2>/dev/null || true
      wait "$relay_pid" 2>/dev/null || true
    fi
  }
  trap cleanup EXIT

  log "Starting macOS APX relay: $RELAY_CMD -b $RELAY_BIND -p $RELAY_PORT"
  "$RELAY_CMD" -b "$RELAY_BIND" -p "$RELAY_PORT" &
  relay_pid="$!"
  wait_for_relay "$relay_pid"

  run_docker_flash "$boot_priority" "$boot_order_overlay"
}

main "$@"
