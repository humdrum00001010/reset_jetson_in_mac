#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BOOT="$ROOT_DIR/boot.sh"

fail() {
  printf 'not ok - %s\n' "$*" >&2
  exit 1
}

assert_contains() {
  local haystack="$1"
  local needle="$2"

  if [[ "$haystack" != *"$needle"* ]]; then
    fail "expected output to contain: $needle"
  fi
}

assert_not_contains() {
  local haystack="$1"
  local needle="$2"

  if [[ "$haystack" == *"$needle"* ]]; then
    fail "expected output not to contain: $needle"
  fi
}

boot_script="$(cat "$BOOT")"
bad_proxy_path="$(printf '/%s/%s' tmp qemu-usbfix)"
assert_not_contains "$boot_script" "$bad_proxy_path"
assert_contains "$boot_script" 'QEMU_I386_PROXY_DEFAULT="$SCRIPT_DIR/bin/qemu-i386-apxproxy"'

temp_boot_dir="$(mktemp -d "${TMPDIR:-/tmp}/boot-test.XXXXXX")"
trap 'rm -rf "$temp_boot_dir"' EXIT
cp "$BOOT" "$temp_boot_dir/boot.sh"
chmod +x "$temp_boot_dir/boot.sh"

missing_proxy_output="$(HOME=/home/jetson-host BOOT_SH_DRY_RUN=1 "$temp_boot_dir/boot.sh" sd ssd)"
assert_contains "$missing_proxy_output" "QEMU_I386_PROXY=<missing: set QEMU_I386_PROXY or install "
assert_contains "$missing_proxy_output" "/bin/qemu-i386-apxproxy>"

dry_run_output="$(HOME=/home/jetson-host RELAY_CMD=./apx-relayd QEMU_I386_PROXY=/opt/qemu-i386-apxproxy BOOT_SH_DRY_RUN=1 "$BOOT" sd ssd)"
assert_contains "$dry_run_output" "DefaultBootPriority=sd,nvme"
assert_contains "$dry_run_output" "QEMU_I386_PROXY=/opt/qemu-i386-apxproxy"
assert_contains "$dry_run_output" "L4T_DIR=/home/jetson-host/jetson-flash/Linux_for_Tegra"

printf 'ok - boot.sh contract\n'
