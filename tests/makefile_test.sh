#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
MAKEFILE="$ROOT_DIR/Makefile"
BOOT="$ROOT_DIR/boot.sh"

fail() {
  printf 'not ok - %s\n' "$*" >&2
  exit 1
}

assert_contains() {
  local haystack="$1"
  local needle="$2"

  if [[ "$haystack" != *"$needle"* ]]; then
    fail "expected text to contain: $needle"
  fi
}

makefile_text="$(cat "$MAKEFILE")"
boot_text="$(cat "$BOOT")"

assert_contains "$makefile_text" "TARGET := bin/apx-relayd"
assert_contains "$makefile_text" "QEMU_PROXY := bin/qemu-i386-apxproxy"
assert_contains "$makefile_text" 'all: $(TARGET) $(QEMU_PROXY)'
assert_contains "$makefile_text" '$(QEMU_PROXY_BUILDER)'
assert_contains "$makefile_text" 'QEMU_ARCHIVE_DIR="$(QEMU_ARCHIVE_DIR)"'
assert_contains "$boot_text" 'RELAY_CMD="${RELAY_CMD:-$SCRIPT_DIR/bin/apx-relayd}"'

builder_text="$(cat "$ROOT_DIR/scripts/build-qemu-i386-apxproxy.sh")"
assert_contains "$builder_text" '.part'
assert_contains "$builder_text" 'mv "$partial_tarball" "$tarball"'
assert_contains "$builder_text" 'QEMU_ARCHIVE_DIR="${QEMU_ARCHIVE_DIR:-qemu-${QEMU_VERSION}}"'
assert_contains "$builder_text" 'archive_name="${QEMU_URL##*/}"'
assert_contains "$builder_text" 'docker run --rm -i'
assert_contains "$builder_text" 'python3-venv'
assert_contains "$builder_text" ' git '

printf 'ok - Makefile contract\n'
