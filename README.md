# Jetson Boot Control From macOS

Mac controls Jetson boot order by relaying the Jetson's USB APX recovery traffic into a Linux flashing container and flashing only the QSPI boot-order overlay. The scripts do not create a Jetson rootfs image; they separate boot-order control from rootfs migration.

Target board: Jetson Orin Nano Super 8GB.

## Build

```sh
make
```

`make` produces the host-side tools under `bin/`:

- `bin/apx-relayd`: macOS USB APX relay server.
- `bin/qemu-i386-apxproxy`: Linux amd64 QEMU user-mode binary patched to proxy usbfs APX access through `apx-relayd`.

The QEMU proxy build runs in Docker and downloads official QEMU source from `https://download.qemu.org/`. To use a specific source tarball:

```sh
QEMU_URL=https://download.qemu.org/qemu-8.2.7.tar.xz make qemu-proxy
```

## Boot Order

`boot.sh` is the Mac-side boot-order controller. Its input is the preferred boot-device order, and its output is a QSPI-only flash that changes Jetson UEFI `DefaultBootPriority`.

```sh
./boot.sh sd ssd
./boot.sh ssd sd
```

Behavior:

- `sd ssd` flashes SD/eMMC before NVMe.
- `ssd sd` flashes NVMe before SD/eMMC.
- The script starts `bin/apx-relayd` on macOS.
- Docker runs the official Jetson Linux `flash.sh` from `L4T_DIR`.
- The container uses `bin/qemu-i386-apxproxy` to turn Linux usbfs APX calls into relay RPCs.
- `flash.sh` is called with `--no-systemimg --qspi-only`, so rootfs contents are not reflashed.

Required inputs:

- Jetson in APX/recovery mode over USB.
- Docker on macOS.
- `L4T_DIR` pointing at an official `Linux_for_Tegra` tree. Default: `$HOME/jetson-flash/Linux_for_Tegra`.

Dry-run:

```sh
BOOT_SH_DRY_RUN=1 ./boot.sh sd ssd
```

## SD To SSD

`to_ssd.sh` is the Mac-side SSD migration launcher. It copies an embedded migration script over SSH, prompts for the Jetson sudo password on macOS, feeds that password to Jetson `sudo`, and stages SD rootfs to the NVMe SSD.

```sh
./to_ssd.sh <jetson-user> <jetson-host>
```

Behavior on the Jetson:

- Verifies it is currently booted from SD/eMMC.
- Repairs and expands the NVMe root partition.
- Copies `/` to the NVMe root partition with `rsync`.
- Rewrites `/boot/extlinux/extlinux.conf` to use the NVMe partition UUID.
- Copies the patched boot config to the NVMe filesystem.
- Disables the old `setssdroot.service` path if present.
- Leaves reboot to the operator.

The Jetson-side migration script must run as root. The macOS wrapper reads the Jetson sudo password with hidden input and does not accept passwords as command-line arguments.

Print only the Jetson-side script:

```sh
./to_ssd.sh --print-jetson-script
```

Open a manual serial console helper:

```sh
./to_ssd.sh --serial [device]
```
