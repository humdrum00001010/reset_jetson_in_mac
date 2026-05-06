#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
README="$ROOT_DIR/README.md"

fail() {
  printf 'not ok - %s\n' "$*" >&2
  exit 1
}

assert_contains() {
  local haystack="$1"
  local needle="$2"

  if [[ "$haystack" != *"$needle"* ]]; then
    fail "expected README to contain: $needle"
  fi
}

text="$(cat "$README" 2>/dev/null || true)"

assert_contains "$text" "Mac controls Jetson boot order"
assert_contains "$text" "Target board: Jetson Orin Nano Super 8GB"
assert_contains "$text" "./boot.sh sd ssd"
assert_contains "$text" "./boot.sh ssd sd"
assert_contains "$text" "./to_ssd.sh <jetson-user> <jetson-host>"
assert_contains "$text" "Mac-side SSD migration launcher"
assert_contains "$text" "prompts for the Jetson sudo password"
assert_contains "$text" "SD rootfs to the NVMe SSD"

printf 'ok - README contract\n'
