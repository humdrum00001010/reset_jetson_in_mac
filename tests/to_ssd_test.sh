#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TO_SSD="$ROOT_DIR/to_ssd.sh"
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

if "$TO_SSD" >/tmp/to-ssd-usage.out 2>&1; then
  fail "expected missing arguments to fail"
fi
assert_contains "$(cat /tmp/to-ssd-usage.out)" "usage: ./to_ssd.sh <jetson-user> <jetson-host>"
assert_contains "$(cat /tmp/to-ssd-usage.out)" "usage: ./to_ssd.sh --serial [device]"

script_text="$(cat "$TO_SSD" "$BOOT")"
assert_not_contains "$script_text" "/Users/"

dry_run_output="$(TO_SSD_DRY_RUN=1 "$TO_SSD" jetson-user jetson-host.local)"
assert_contains "$dry_run_output" "remote=jetson-user@jetson-host.local"
assert_contains "$dry_run_output" "ssh jetson-user@jetson-host.local"
assert_contains "$dry_run_output" "prompt: read Jetson sudo password on macOS (hidden)"
assert_contains "$dry_run_output" "sudo -k -S -p '' bash /tmp/sd-to-ssd.sh"

serial_dry_run_output="$(TO_SSD_DRY_RUN=1 "$TO_SSD" --serial /dev/cu.usbmodem-test)"
assert_contains "$serial_dry_run_output" "serial=/dev/cu.usbmodem-test"
assert_contains "$serial_dry_run_output" "screen /dev/cu.usbmodem-test 115200"
assert_contains "$serial_dry_run_output" "manual login"
assert_contains "$serial_dry_run_output" "passwords are not accepted as script arguments"

jetson_script="$("$TO_SSD" --print-jetson-script)"
assert_contains "$script_text" "read -rs"
assert_contains "$script_text" "sudo -k -S -p '' bash"
assert_contains "$jetson_script" "run as root: sudo"
assert_contains "$jetson_script" "NVME_ROOT_PART=\"\${NVME_ROOT_PART:-/dev/nvme0n1p1}\""
assert_contains "$jetson_script" "e2fsck -fy"
assert_contains "$jetson_script" "resize2fs"
assert_contains "$jetson_script" "rsync -axHAWX --numeric-ids --delete"
assert_contains "$jetson_script" "root=UUID="
assert_contains "$jetson_script" "systemctl disable setssdroot.service"

printf 'ok - to_ssd.sh contract\n'
