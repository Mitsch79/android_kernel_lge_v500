# V500 kernel port for LineageOS 21

This branch carries the V500 board sources and patch series for Google's `android14-6.1` at `b07b626eb82251b596ef1812adf7c33ad6e4f16e`. The [manifest](https://github.com/Mitsch79/android_local_manifests_v500/tree/lineage-21.0) places this branch at `kernel/lge/v500-port` and the unmodified kernel at `kernel/google/android14-6.1`. The build applies the series in a separate, locked Git worktree. Shared board files under `Kernels/6.18` are inputs to the 6.1 build.

## Building a test boot image

The LD083WU1 panel controls its LM3532 illumination through the Linux backlight class at `/sys/class/backlight/panel-backlight`. DRM blanks the light when disabling the display and restores its selected brightness when enabling it.

Build Android's system, vendor and ramdisk images first. Set the following environment variables from the Android 21 source root.

```sh
export V500_KERNEL_UPSTREAM="$PWD/kernel/google/android14-6.1"
export V500_M6_RAMDISK="${OUT_DIR:-$PWD/out}/target/product/v500/ramdisk.img"
export V500_MAINLINE_WORK="$HOME/android/v500-mainline"
export LINEAGE_SOURCE_DIR="$HOME/android/lineage14"
export V500_REFERENCE_BOOT="$HOME/v500-reference/boot.img"
bash kernel/lge/v500-port/Kernels/6.1/build.sh lineage21
```

The boot packer expects a matching LG V500 reference boot image, the 14.1 tree's host `mkbootimg` and `device/lge/gpad83-common/bump.py`, and locally extracted `a300_pfp.fw` and `a300_pm4.fw` under `vendor/lge/v500/proprietary/vendor/firmware` in that tree. Prepare a static ARM BusyBox at `$V500_MAINLINE_WORK/busybox-armhf/bin/busybox`. Host tools include an ARM hard-float cross-compiler, kernel build prerequisites, device-tree-compiler, Python 2 and 3, and qemu-arm.

The resulting M6 test boot uses the V500 recovery timer and Android userdebug configuration. The export includes its SHA-256 list, effective kernel configuration and device tree. M6 staging uses separate ext4 test images. A recovery-installable daily-use ROM is a later milestone.
