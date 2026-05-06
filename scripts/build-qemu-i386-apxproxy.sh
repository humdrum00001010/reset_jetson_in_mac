#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

QEMU_VERSION="${QEMU_VERSION:-8.2.7}"
QEMU_URL="${QEMU_URL:-https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz}"
QEMU_ARCHIVE_DIR="${QEMU_ARCHIVE_DIR:-qemu-${QEMU_VERSION}}"
QEMU_BUILD_IMAGE="${QEMU_BUILD_IMAGE:-ubuntu:22.04}"
QEMU_BUILD_PLATFORM="${QEMU_BUILD_PLATFORM:-linux/amd64}"
OUTPUT="${OUTPUT:-bin/qemu-i386-apxproxy}"

case "$OUTPUT" in
  "$ROOT_DIR"/*) OUTPUT_REL="${OUTPUT#"$ROOT_DIR"/}" ;;
  /*)
    printf '[qemu-proxy] ERROR: OUTPUT must be inside the repository: %s\n' "$OUTPUT" >&2
    exit 1
    ;;
  *) OUTPUT_REL="$OUTPUT" ;;
esac

if [ "${QEMU_PROXY_DRY_RUN:-0}" = "1" ]; then
  cat <<EOF
qemu version: $QEMU_VERSION
qemu source: $QEMU_URL
qemu archive dir: $QEMU_ARCHIVE_DIR
docker image: $QEMU_BUILD_IMAGE ($QEMU_BUILD_PLATFORM)
output: $OUTPUT_REL
EOF
  exit 0
fi

command -v docker >/dev/null 2>&1 || {
  printf '[qemu-proxy] ERROR: docker is not in PATH\n' >&2
  exit 1
}

mkdir -p "$ROOT_DIR/bin" "$ROOT_DIR/build/qemu-i386-apxproxy"

docker run --rm -i \
  --platform "$QEMU_BUILD_PLATFORM" \
  -v "$ROOT_DIR:/work" \
  -e "QEMU_VERSION=$QEMU_VERSION" \
  -e "QEMU_URL=$QEMU_URL" \
  -e "QEMU_ARCHIVE_DIR=$QEMU_ARCHIVE_DIR" \
  -e "OUTPUT_REL=$OUTPUT_REL" \
  "$QEMU_BUILD_IMAGE" \
  bash -s <<'CONTAINER_SCRIPT'
set -euo pipefail

log() {
  printf '[qemu-proxy] %s\n' "$*" >&2
}

export DEBIAN_FRONTEND=noninteractive
log "Installing QEMU build dependencies"
apt-get update >/dev/null
apt-get install -y --no-install-recommends \
  build-essential ca-certificates curl git libglib2.0-dev libpixman-1-dev \
  ninja-build pkg-config python3 python3-venv xz-utils zlib1g-dev >/dev/null

work=/work/build/qemu-i386-apxproxy
archive_name="${QEMU_URL##*/}"
archive_name="${archive_name%%\?*}"
tarball="$work/$archive_name"
partial_tarball="${tarball}.part"
src="$work/$QEMU_ARCHIVE_DIR"

mkdir -p "$work"
if [ ! -s "$tarball" ]; then
  log "Downloading official QEMU source: $QEMU_URL"
  rm -f "$partial_tarball"
  curl -L --fail --retry 3 --connect-timeout 20 -o "$partial_tarball" "$QEMU_URL"
  mv "$partial_tarball" "$tarball"
fi

if [ ! -d "$src" ]; then
  log "Extracting qemu-${QEMU_VERSION}"
  tar -C "$work" -xf "$tarball"
fi

cp /work/src/apx_qemu_usbfs_proxy.c "$src/linux-user/apx_qemu_usbfs_proxy.c"

python3 - "$src/linux-user/syscall.c" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text()

def replace_once(needle, replacement):
    global text
    if replacement in text:
        return
    count = text.count(needle)
    if count != 1:
        raise SystemExit(f"expected one match, found {count}: {needle[:80]!r}")
    text = text.replace(needle, replacement, 1)

replace_once(
    '#endif /* CONFIG_USBFS */\n\nstatic abi_long do_ioctl_dm',
    '#endif /* CONFIG_USBFS */\n\n#if defined(CONFIG_USBFS)\n#include "apx_qemu_usbfs_proxy.c"\n#endif\n\nstatic abi_long do_ioctl_dm',
)

replace_once(
    '    void *argptr;\n\n    ie = ioctl_entries;',
    '    void *argptr;\n\n#if defined(CONFIG_USBFS)\n    if (apx_qemu_usbfs_proxy_is_fd(fd)) {\n        abi_long apx_proxy_ret;\n\n        apx_proxy_ret = apx_qemu_usbfs_proxy_do_ioctl_direct(fd, cmd, arg);\n        if (apx_proxy_ret != APX_USBFS_PROXY_IOCTL_NOT_HANDLED) {\n            return apx_proxy_ret;\n        }\n    }\n#endif\n\n    ie = ioctl_entries;',
)

replace_once(
    '    arg_type = ie->arg_type;\n    if (ie->do_ioctl) {',
    '    arg_type = ie->arg_type;\n#if defined(CONFIG_USBFS)\n    if (apx_qemu_usbfs_proxy_is_fd(fd)) {\n        return apx_qemu_usbfs_proxy_do_ioctl(ie, buf_temp, fd, cmd, arg);\n    }\n#endif\n    if (ie->do_ioctl) {',
)

replace_once(
    '    if (is_proc_myself(pathname, "exe")) {\n        if (safe) {\n            return safe_openat(dirfd, exec_path, flags, mode);\n        } else {\n            return openat(dirfd, exec_path, flags, mode);\n        }\n    }\n\n    for (fake_open = fakes; fake_open->filename; fake_open++) {',
    '    if (is_proc_myself(pathname, "exe")) {\n        if (safe) {\n            return safe_openat(dirfd, exec_path, flags, mode);\n        } else {\n            return openat(dirfd, exec_path, flags, mode);\n        }\n    }\n\n#if defined(CONFIG_USBFS)\n    {\n        int apx_proxy_fd = apx_qemu_usbfs_proxy_open(pathname, flags);\n        if (apx_proxy_fd != APX_USBFS_PROXY_NOT_HANDLED) {\n            return apx_proxy_fd;\n        }\n    }\n#endif\n\n    for (fake_open = fakes; fake_open->filename; fake_open++) {',
)

replace_once(
    '            if (!(p = lock_user(VERIFY_WRITE, arg2, arg3, 0)))\n                return -TARGET_EFAULT;\n            ret = get_errno(safe_read(arg1, p, arg3));',
    '            if (!(p = lock_user(VERIFY_WRITE, arg2, arg3, 0)))\n                return -TARGET_EFAULT;\n#if defined(CONFIG_USBFS)\n            if (apx_qemu_usbfs_proxy_is_fd(arg1)) {\n                ret = apx_qemu_usbfs_proxy_read(arg1, p, arg3);\n                unlock_user(p, arg2, ret > 0 ? ret : 0);\n                return ret;\n            }\n#endif\n            ret = get_errno(safe_read(arg1, p, arg3));',
)

replace_once(
    '    case TARGET_NR_close:\n        fd_trans_unregister(arg1);\n        return get_errno(close(arg1));',
    '    case TARGET_NR_close:\n#if defined(CONFIG_USBFS)\n        if (apx_qemu_usbfs_proxy_is_fd(arg1)) {\n            fd_trans_unregister(arg1);\n            return apx_qemu_usbfs_proxy_close(arg1);\n        }\n#endif\n        fd_trans_unregister(arg1);\n        return get_errno(close(arg1));',
)

path.write_text(text)
PY

build_dir="$src/build-apxproxy"
mkdir -p "$build_dir"
cd "$build_dir"

if [ ! -f build.ninja ]; then
  log "Configuring QEMU i386 linux-user"
  ../configure \
    --target-list=i386-linux-user \
    --disable-system \
    --disable-tools \
    --disable-docs \
    --disable-gtk \
    --disable-sdl \
    --disable-vnc \
    --disable-curses \
    --disable-opengl \
    --disable-slirp \
    --disable-guest-agent \
    --disable-werror
fi

log "Building qemu-i386 APX proxy"
make -j"$(nproc)" qemu-i386
install -m 0755 qemu-i386 "/work/$OUTPUT_REL"
log "Wrote /work/$OUTPUT_REL"
CONTAINER_SCRIPT
