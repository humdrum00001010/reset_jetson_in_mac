#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PROXY_SRC="$ROOT_DIR/src/apx_qemu_usbfs_proxy.c"

fail() {
  printf 'not ok - %s\n' "$*" >&2
  exit 1
}

source_text="$(cat "$PROXY_SRC")"
control_raw_text="$(sed -n '/apx_usbfs_proxy_ioctl_control_raw/,/apx_usbfs_proxy_ioctl_bulk_raw/p' "$PROXY_SRC")"
control_thunk_text="$(sed -n '/apx_usbfs_proxy_ioctl_control(/,/apx_usbfs_proxy_ioctl_bulk(/p' "$PROXY_SRC")"

if [[ "$source_text" == *"apx_usbfs_proxy_target_errno_from_status"* ]]; then
  fail "unused status-to-target errno helper should not be present"
fi

if [[ "$control_raw_text" == *"if (length > APX_USBFS_PROXY_MAX_TRANSFER)"* ]]; then
  fail "16-bit control length should not be compared to 16 MiB max transfer"
fi

if [[ "$control_thunk_text" == *"if (ctrl->wLength > APX_USBFS_PROXY_MAX_TRANSFER)"* ]]; then
  fail "16-bit control wLength should not be compared to 16 MiB max transfer"
fi

printf 'ok - proxy source warnings\n'
