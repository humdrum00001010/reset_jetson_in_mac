#!/usr/bin/env bash
set -euo pipefail

REMOTE_SCRIPT="${TO_SSD_REMOTE_SCRIPT:-/tmp/sd-to-ssd.sh}"
SERIAL_BAUD="${TO_SSD_SERIAL_BAUD:-115200}"

log() {
  printf '[to-ssd] %s\n' "$*" >&2
}

die() {
  printf '[to-ssd] ERROR: %s\n' "$*" >&2
  exit 1
}

usage() {
  printf 'usage: ./to_ssd.sh <jetson-user> <jetson-host>\n' >&2
  printf 'usage: ./to_ssd.sh --serial [device]\n' >&2
}

print_jetson_script() {
  cat <<'JETSON_SCRIPT'
#!/usr/bin/env bash
set -euo pipefail

NVME_ROOT_PART="${NVME_ROOT_PART:-/dev/nvme0n1p1}"
MOUNT_POINT="${MOUNT_POINT:-/mnt}"
SD_EXTLINUX="${SD_EXTLINUX:-/boot/extlinux/extlinux.conf}"

log() {
  printf '[sd-to-ssd] %s\n' "$*" >&2
}

die() {
  printf '[sd-to-ssd] ERROR: %s\n' "$*" >&2
  exit 1
}

require_root() {
  if [ "$(id -u)" -ne 0 ]; then
    die "run as root: sudo ./sd-to-ssd.sh"
  fi
}

require_commands() {
  local command_name
  for command_name in blkid cp df e2fsck findmnt grep mkdir mount resize2fs rsync sed sync test umount; do
    command -v "$command_name" >/dev/null 2>&1 || die "missing command: $command_name"
  done
}

current_root_source() {
  findmnt -no SOURCE /
}

target_uuid() {
  blkid -s UUID -o value "$NVME_ROOT_PART"
}

ensure_sd_to_nvme_shape() {
  local root_source="$1"

  [ -b "$NVME_ROOT_PART" ] || die "missing NVMe root partition: $NVME_ROOT_PART"

  if [ "$root_source" = "$NVME_ROOT_PART" ]; then
    log "rootfs is already on $NVME_ROOT_PART"
    exit 0
  fi

  case "$root_source" in
    /dev/mmcblk*|/dev/disk/by-uuid/*) ;;
    *) die "expected current rootfs to be SD/eMMC, got: $root_source" ;;
  esac
}

prepare_nvme_rootfs() {
  if findmnt "$MOUNT_POINT" >/dev/null 2>&1; then
    local mounted_source
    mounted_source="$(findmnt -no SOURCE "$MOUNT_POINT")"
    [ "$mounted_source" = "$NVME_ROOT_PART" ] || die "$MOUNT_POINT is mounted from $mounted_source"
    sync
    umount "$MOUNT_POINT"
  fi

  log "Repairing and expanding $NVME_ROOT_PART"
  e2fsck -fy "$NVME_ROOT_PART"
  resize2fs "$NVME_ROOT_PART"
  e2fsck -fy "$NVME_ROOT_PART"

  mkdir -p "$MOUNT_POINT"
  mount "$NVME_ROOT_PART" "$MOUNT_POINT"
}

copy_rootfs() {
  log "Copying SD rootfs to $NVME_ROOT_PART"
  rsync -axHAWX --numeric-ids --delete --info=stats2 \
    --exclude={"/dev/","/proc/","/sys/","/tmp/","/run/","/mnt/","/media/*","/lost+found"} \
    / "$MOUNT_POINT"

  test -d "$MOUNT_POINT/home"
  test -d "$MOUNT_POINT/usr"
  test -d "$MOUNT_POINT/etc"
  test -d "$MOUNT_POINT/boot"
}

patch_sd_boot_root() {
  local nvme_uuid="$1"
  local backup="${SD_EXTLINUX}.sdroot.bak"

  [ -f "$SD_EXTLINUX" ] || die "missing boot config: $SD_EXTLINUX"
  grep -q 'root=' "$SD_EXTLINUX" || die "boot config has no root= entry: $SD_EXTLINUX"

  [ -f "$backup" ] || cp -a "$SD_EXTLINUX" "$backup"
  sed -i -E "s#root=[^[:space:]]+#root=UUID=${nvme_uuid}#" "$SD_EXTLINUX"
}

copy_boot_config_to_nvme() {
  mkdir -p "$MOUNT_POINT/boot/extlinux"
  cp -a "$SD_EXTLINUX" "$MOUNT_POINT/boot/extlinux/extlinux.conf"
  cp -a "${SD_EXTLINUX}.sdroot.bak" "$MOUNT_POINT/boot/extlinux/extlinux.conf.sdroot.bak" 2>/dev/null || true
}

disable_switch_root_service() {
  if command -v systemctl >/dev/null 2>&1; then
    systemctl disable setssdroot.service >/dev/null 2>&1 || true
  fi
  rm -f /etc/setssdroot.conf "$MOUNT_POINT/etc/setssdroot.conf" 2>/dev/null || true
}

finish_migration() {
  sync
  df -h "$MOUNT_POINT"
  umount "$MOUNT_POINT"
  log "Migration staged. Reboot the Jetson, then verify: findmnt -no SOURCE /"
}

main() {
  require_root
  require_commands

  local root_source
  root_source="$(current_root_source)"
  ensure_sd_to_nvme_shape "$root_source"

  local nvme_uuid
  nvme_uuid="$(target_uuid)"
  [ -n "$nvme_uuid" ] || die "could not read UUID for $NVME_ROOT_PART"

  prepare_nvme_rootfs
  copy_rootfs
  patch_sd_boot_root "$nvme_uuid"
  copy_boot_config_to_nvme
  disable_switch_root_service
  finish_migration
}

main "$@"
JETSON_SCRIPT
}

validate_remote_script_path() {
  case "$REMOTE_SCRIPT" in
    /tmp/*) ;;
    *) die "TO_SSD_REMOTE_SCRIPT must be under /tmp" ;;
  esac

  if [[ ! "$REMOTE_SCRIPT" =~ ^[A-Za-z0-9_./-]+$ ]]; then
    die "TO_SSD_REMOTE_SCRIPT contains unsupported characters"
  fi
}

print_dry_run() {
  local remote="$1"

  cat <<EOF
remote=$remote
copy: print embedded Jetson script | ssh $remote 'umask 077; cat > $REMOTE_SCRIPT && chmod 700 $REMOTE_SCRIPT'
prompt: read Jetson sudo password on macOS (hidden)
run: printf password | ssh $remote "sudo -k -S -p '' bash $REMOTE_SCRIPT"
EOF
}

read_jetson_sudo_password() {
  local password

  [ -t 0 ] || die "cannot read Jetson sudo password without a terminal"
  printf 'Jetson sudo password: ' >&2
  IFS= read -rs password
  printf '\n' >&2

  [ -n "$password" ] || die "empty Jetson sudo password"
  printf '%s\n' "$password"
}

detect_serial_device() {
  local device
  device="$(find /dev -maxdepth 1 \( -name 'cu.usbmodem*' -o -name 'tty.usbmodem*' \) 2>/dev/null | sort | head -n 1)"
  [ -n "$device" ] || die "no Jetson USB serial device found"
  printf '%s\n' "$device"
}

print_serial_dry_run() {
  local serial_device="$1"

  cat <<EOF
serial=$serial_device
run: screen $serial_device $SERIAL_BAUD
note: manual login only; passwords are not accepted as script arguments
EOF
}

run_serial_console() {
  local serial_device="$1"

  command -v screen >/dev/null 2>&1 || die "screen is not in PATH"
  log "Opening Jetson serial console: screen $serial_device $SERIAL_BAUD"
  log "Use manual login. This mode does not receive or store passwords."
  log "Exit screen with Ctrl-A then K."
  exec screen "$serial_device" "$SERIAL_BAUD"
}

run_remote_migration() {
  local remote="$1"
  local jetson_sudo_password="$2"

  log "Copying Jetson migration script to $remote:$REMOTE_SCRIPT"
  print_jetson_script | ssh "$remote" "umask 077; cat > $REMOTE_SCRIPT && chmod 700 $REMOTE_SCRIPT"

  log "Starting Jetson-side migration with Jetson sudo password supplied from macOS."
  printf '%s\n' "$jetson_sudo_password" | ssh "$remote" "sudo -k -S -p '' bash $REMOTE_SCRIPT"
}

main() {
  if [ "${1:-}" = "--print-jetson-script" ]; then
    print_jetson_script
    return 0
  fi

  if [ "${1:-}" = "--serial" ]; then
    if [ "$#" -gt 2 ]; then
      usage
      exit 2
    fi

    local serial_device="${2:-}"
    if [ -z "$serial_device" ]; then
      serial_device="$(detect_serial_device)"
    fi

    if [ "${TO_SSD_DRY_RUN:-0}" = "1" ]; then
      print_serial_dry_run "$serial_device"
      return 0
    fi

    run_serial_console "$serial_device"
    return 0
  fi

  if [ "$#" -ne 2 ]; then
    usage
    exit 2
  fi

  local jetson_user="$1"
  local jetson_host="$2"
  [ -n "$jetson_user" ] || die "missing Jetson user"
  [ -n "$jetson_host" ] || die "missing Jetson host"
  command -v ssh >/dev/null 2>&1 || die "ssh is not in PATH"
  validate_remote_script_path

  local remote="${jetson_user}@${jetson_host}"
  if [ "${TO_SSD_DRY_RUN:-0}" = "1" ]; then
    print_dry_run "$remote"
    return 0
  fi

  local jetson_sudo_password
  jetson_sudo_password="$(read_jetson_sudo_password)"
  run_remote_migration "$remote" "$jetson_sudo_password"
}

main "$@"
