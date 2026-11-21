# V500 kernel port for LineageOS 21

This branch carries the V500 board sources and patch series for Google's `android14-6.1` at `b07b626eb82251b596ef1812adf7c33ad6e4f16e`. The [manifest](https://github.com/Mitsch79/android_local_manifests_v500/tree/lineage-21.0) places this branch at `kernel/lge/v500-port` and the unmodified kernel at `kernel/google/android14-6.1`. The build applies the series in a separate, locked Git worktree. Shared board files under `Kernels/6.18` are inputs to the 6.1 build.

## Building a test boot image

The Android test configuration enables high-resolution timers on the Qualcomm one-shot clock event device. The kernel tick rate remains 100 Hz.

`V500_M6_VBIF_CANCEL_IDLE=1` builds an experimental V500 Adreno driver in a separate worktree. An incoming runtime power reference can interrupt the wait for bus clients during autosuspend. The driver still requires all six acknowledgements before powering down.

The PM8921 driver reports cell voltage, battery temperature and net cell current. Android builds enable budgeted USB charging by default. Set `V500_USB_CHARGING=0` when building the passive GUI test profile. The standard USB budget is capped at 500 mA. The higher CDP budget requires an explicit test build. Sustained net charging at 1.5 A with the display on remains unverified.

With VBUS present, `usb/voltage_now` reports the PM8921 USBIN ADC reading in microvolts. The input starts at 100 mA and waits at least two seconds between increases. A reading below 4.520 V holds the initial step or lowers an established limit by one calibrated step. A reduced ceiling persists until an observed disconnect. The threshold and the 100 ms follow-up check derive from the AWIFI dedicated-charger routine. This port also applies the check to data ports. The regulator overshoot interrupt still inhibits charging until VBUS removal.

A standard data-port test reached 500 mA with this pacing and then tripped the regulator overshoot interlock. Sustained charging at this step remains under investigation.

During startup the port first programs the input limit and trim, releases USB suspend, and waits for the input to settle before setting automatic cell charging. This follows the ordering in the LG charger initialization. The standard profile records the PM8921 charger state on a regulator overshoot and inhibits the input.

The optional timed Android measurement profile `V500_TEST_SDP_INPUT_EDGE=1` checks one rising edge within 100 ms of reaching 500 mA on a detected standard data port. It requires the automatic charger off, an open battery FET, a cleared regulator signal, charger state 1, valid temperature and supply readings, and a cell voltage below the 4.20 V target. The driver keeps the cell charger off while it rechecks the input and FET. A second edge or a failed observation inhibits the input. This profile excludes high-current CDP and wall-charger sessions.

The timed profile sustained its 500 mA input limit and positive net cell current in a short screen-off data-port test. Full-cycle qualification remains open.

The LD083WU1 panel controls its LM3532 illumination through the Linux backlight class at `/sys/class/backlight/panel-backlight`. DRM blanks the light when disabling the display and restores its selected brightness when enabling it.

During GPU runtime suspend, the V500 Adreno 320 driver waits up to 100 ms for all six VBIF clients to acknowledge the halt request. If any acknowledgment is missing, it releases the request and returns busy. This bounds the time a newly submitted frame can spend behind this particular poll.

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

The default M6 boot uses the V500 recovery timer. For a manual GUI session, set `V500_M6_MANUAL_SESSION=1` when building the LineageOS 21 boot image. After the loop devices are ready, an initramfs helper takes over the hardware watchdog and feeds it while the battery reports good health, a charge of at least 30% and a temperature from 10 to below 42 °C. The session has no elapsed-time cutoff. The tested GUI profile uses `V500_USB_CHARGING=0`.

The export includes its SHA-256 list, effective kernel configuration and device tree. M6 staging uses separate ext4 test images.
