#!/usr/bin/env bash
set -euo pipefail
port=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
here="$port/../6.18"
work=${V500_MAINLINE_WORK:-$HOME/android/v500-mainline}
revision=b07b626eb82251b596ef1812adf7c33ad6e4f16e
source_dir="$work/v500-port-6.1.177"
build_dir="$work/build-6.1.177-v500"
probe=${1:-lineage21}
case "$probe" in trace|lineage21) ;; *) echo "Usage: $0 [trace|lineage21]" >&2; exit 2 ;; esac
vbif_cancel_idle=${V500_M6_VBIF_CANCEL_IDLE:-0}
case "$vbif_cancel_idle" in
    0) ;;
    1) [[ $probe == lineage21 ]]; source_dir+=-vbif-resume; build_dir+=-vbif-resume ;;
    *) echo 'V500_M6_VBIF_CANCEL_IDLE must be 0 or 1' >&2; exit 2 ;;
esac
export_dir="${V500_EXPORT_DIR:-$port/out}/probes/$probe"
lineage=${LINEAGE_SOURCE_DIR:-$HOME/android/lineage}
m6="$port/../../lineage21"
reference=${V500_REFERENCE_BOOT:-$port/../../reference/lineage-14.1-20220320-UNOFFICIAL-v500/boot.img}
busybox="$work/busybox-armhf/bin/busybox"
mkbootimg="$lineage/out/host/linux-x86/bin/mkbootimg"
bump="$lineage/device/lge/gpad83-common/bump.py"
android_ramdisk=${V500_M6_RAMDISK:-$m6/out/native-m6-vendor-kernel/ramdisk.img}
mkdir -p "$build_dir/board" "$export_dir"
exec 9>"$build_dir/.lock"
flock 9
if [[ ! -e $source_dir ]]; then
    git -C "${V500_KERNEL_UPSTREAM:-$work/android14-6.1-$revision}" worktree add --detach "$source_dir" "$revision"
fi
[[ $(git -C "$source_dir" rev-parse HEAD) == "$revision" ]]
changes=(v500-arm-boot v500-peripherals v500-maxtouch-fifo v500-arm-bpf-atomic-add
         v500-arm-bpf-div64 v500-a3xx-devfreq v500-panel-resume v500-panel-backlight
         v500-a320-vbif-timeout)
if [[ $vbif_cancel_idle == 1 ]]; then changes+=(v500-a320-vbif-resume); fi
changes+=(v500-msm8960-usb-phy-reset-id v500-battery v500-awifi-xoadc)
kernel_git=(git -C "$source_dir")
expected_index="$build_dir/v500-series.index"
series_patch="$build_dir/v500-series.patch"
GIT_INDEX_FILE="$expected_index" "${kernel_git[@]}" read-tree "$revision"
applied=0
for i in "${!changes[@]}"; do
    GIT_INDEX_FILE="$expected_index" "${kernel_git[@]}" apply --cached "$port/${changes[i]}.patch"
    GIT_INDEX_FILE="$expected_index" "${kernel_git[@]}" diff --cached --binary "$revision" > "$series_patch"
    # A later patch can edit a file introduced by an earlier patch.
    if "${kernel_git[@]}" apply --reverse --check "$series_patch" 2>/dev/null; then
        applied=$((i + 1))
    fi
done
for change in "${changes[@]:applied}"; do
    "${kernel_git[@]}" apply "$port/$change.patch"
done
"${kernel_git[@]}" apply --reverse --check "$series_patch"
install -m644 "$here/v500-boot-probe.h" "$source_dir/arch/arm/include/asm/"
install -m644 "$here/extcon-pm8921-usb.c" "$source_dir/drivers/extcon/"
install -m644 "$port/v500-battery.c" "$port/v500-battery-data.h" "$source_dir/drivers/power/supply/"
install -m644 "$port/v500-battery.h" "$source_dir/include/linux/power/"
cp "$here"/qcom-apq8064-lg-v500*.dts "$build_dir/board/"
cp "$source_dir/arch/arm/boot/dts"/qcom-apq8064{,-v2.0}.dtsi "$build_dir/board/"
patch --batch --fuzz=0 -d "$build_dir/board" -p1 < "$port/v500-board-6.1.patch"
dts=qcom-apq8064-lg-v500-display
cat "$port/v500-battery.dtsi" >> "$build_dir/board/$dts.dts"
usb_charging=${V500_USB_CHARGING:-}
if [[ -z $usb_charging ]]; then
    if [[ $probe == lineage21 ]]; then usb_charging=1; else usb_charging=0; fi
fi
case $usb_charging in
    0) ;;
    1) printf '\n&v500_battery { lge,enable-usb-charging; };\n' >> "$build_dir/board/$dts.dts" ;;
    *) echo 'V500_USB_CHARGING must be 0 or 1' >&2; exit 2 ;;
esac
dtb="$build_dir/board/$dts.dtb"
cpp -nostdinc -undef -x assembler-with-cpp -D__DTS__ -I "$source_dir/arch/arm/boot/dts" -I "$source_dir/include" \
    "$build_dir/board/$dts.dts" -o "$build_dir/board/display.pp"
dtc -I dts -O dtb -o "$dtb" "$build_dir/board/display.pp"
mmc=$(fdtget "$dtb" /aliases mmc0)
fdtput -d "$dtb" "$mmc" dmas dma-names
read -ra banks <<< "$(fdtget -tx "$dtb" /memory@80200000 reg)"
for node in $(fdtget -l "$dtb" /); do
    case "$node" in memory|memory@0) fdtput -ts "$dtb" "/$node" status disabled ;; esac
done
fdtput -tx "$dtb" /memory@80200000 linux,usable-memory "${banks[@]}"
if [[ $probe == trace ]]; then
    recovery_seconds=65
    bootargs="rdinit=/init v500_trace=1 clk_ignore_unused initcall_debug loglevel=7 console=ttyMSM0,115200n8 panic=8"
    fdtput -ts "$dtb" /soc/mdp@5100000 status disabled
    fdtput -ts "$dtb" /soc/gpu@4300000 status disabled
else
    recovery_seconds=${V500_M6_RECOVERY_SECONDS:-390}
    if [[ ! $recovery_seconds =~ ^[1-9][0-9]{2}$ ]] || (( recovery_seconds < 390 || recovery_seconds > 900 )); then
        echo 'V500_M6_RECOVERY_SECONDS must be between 390 and 900' >&2
        exit 2
    fi
    [[ $(fdtget -tu "$dtb" /soc/gpu@4300000/opp-table/opp-400000000 opp-hz) == '0 400000000' ]]
    bootargs="rdinit=/v500-m6/bootstrap androidboot.hardware=v500 androidboot.v500_gpu=1"
    if [[ ${V500_M6_ADRENO_FIXED_CLOCK:-0} == 1 ]]; then
        for frequency in 128000000 200000000 320000000; do
            fdtput -r "$dtb" "/soc/gpu@4300000/opp-table/opp-$frequency"
        done
        bootargs+=" androidboot.v500_gpu_fixed=1"
    fi
    bootargs+=" clk_ignore_unused vmalloc=400M androidboot.selinux=permissive printk.devkmsg=on log_buf_len=2M loglevel=7 console=ttyMSM0,115200n8 panic=8"
fi
fdtput -ts "$dtb" /chosen bootargs "$bootargs"

cross=${CROSS_COMPILE:-arm-linux-gnueabihf-}
userspace_cppflags=()
if command -v "${cross}gcc" >/dev/null; then
    compiler=("${cross}gcc")
else
    compiler=("$work/toolchain/usr/bin/arm-linux-gnueabihf-gcc-11" -B/usr/lib/gcc-cross/arm-linux-gnueabihf/11/
              -B/usr/arm-linux-gnueabihf/bin/ -B/usr/arm-linux-gnueabihf/lib/)
    userspace_cppflags=(-isystem /usr/arm-linux-gnueabihf/include)
fi
export KBUILD_BUILD_USER=v500 KBUILD_BUILD_HOST=v500 KBUILD_BUILD_TIMESTAMP='2026-11-10 00:00:00 UTC' LOCALVERSION=""
build=(make -C "$source_dir" O="$build_dir" ARCH=arm CROSS_COMPILE="$cross" "CC=${compiler[*]}"
       "KAFLAGS=-DLG_V500_ZRELADDR=0x80208000 -DV500_BOOT_PROBE=4 -DV500_RECOVERY_TIMEOUT=$recovery_seconds")
config="$build_dir/v500.config"
cat "$here/v500-test.config" "$here/v500-stable.config" "$here/v500-display.config" \
    "$here/v500-android.config" "$here/v500-lineage21.config" "$port/v500-6.1.config" > "$config"
atomic_module=
if [[ ${V500_TEST_ATOMIC_ADD:-0} == 1 ]]; then
    [[ $probe == trace ]]
    printf 'CONFIG_RUNTIME_TESTING_MENU=y\nCONFIG_TEST_BPF=m\nCONFIG_MODULE_UNLOAD=y\n' >> "$config"
    atomic_module="$build_dir/lib/test_bpf.ko"
fi
if [[ $probe == lineage21 ]]; then
    # Android's frame scheduler and the GPU driver's sleepable waits need sub-tick timers.
    printf 'CONFIG_HIGH_RES_TIMERS=y\n' >> "$config"
    sed -i '/^CONFIG_USB_G_SERIAL=/d' "$config"
    fw="$build_dir/firmware"
    install -d "$fw/qcom"
    install -m644 "$lineage"/vendor/lge/v500/proprietary/vendor/firmware/a300_{pfp,pm4}.fw "$fw/qcom/"
    printf 'CONFIG_EXTRA_FIRMWARE="qcom/a300_pm4.fw qcom/a300_pfp.fw"\nCONFIG_EXTRA_FIRMWARE_DIR="%s"\n' "$fw" >> "$config"
fi
"${build[@]}" KCONFIG_ALLCONFIG="$config" allnoconfig
if [[ $probe == lineage21 ]]; then grep -Fx 'CONFIG_HIGH_RES_TIMERS=y' "$build_dir/.config"; fi
for symbol in ARCH_QCOM ARCH_MSM8960 CLKSRC_QCOM ARM_APPENDED_DTB ARM_ATAG_DTB_COMPAT HIGHMEM SMP MMC_ARMMMCI \
              ANDROID_BINDER_IPC ANDROID_BINDERFS ASHMEM EXT4_FS USB_CHIPIDEA_MSM EXTCON_PM8921_USB PSTORE_RAM \
              TOUCHSCREEN_ATMEL_MXT LEDS_LM3532 DRM_MSM_MDP4 DRM_MSM_DSI_28NM_8960_PHY DRM_PANEL_LG_LD083WU1 \
              BACKLIGHT_LED MSM_IOMMU DRM_VGEM BPF_JIT CPUSETS USB_CONFIGFS_F_FS USB_CONFIGFS_UEVENT \
              USB_CONFIGFS_F_ACC DMABUF_HEAPS_SYSTEM BATTERY_LG_V500 QCOM_PM8XXX_XOADC; do
    grep -Fx "CONFIG_$symbol=y" "$build_dir/.config" || { echo "Missing $symbol" >&2; exit 1; }
done
grep -Fx 'CONFIG_USB_GADGET_VBUS_DRAW=500' "$build_dir/.config"
"${build[@]}" -j"${V500_JOBS:-12}" zImage
if [[ -n $atomic_module ]]; then
    "${build[@]}" -j"${V500_JOBS:-12}" modules
    test -s "$atomic_module"
fi
tag=$("${build[@]}" -s --no-print-directory kernelrelease)-$probe
source "$here/../pack-v500-boot.sh"
