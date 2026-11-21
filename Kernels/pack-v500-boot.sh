# Shared LG boot envelope and ramdisk construction for V500 test kernels.
zimage="$build_dir/arch/arm/boot/zImage"
work_image="$build_dir/boot-$tag.img"
kernel="$build_dir/zImage-dtb"
ramdisk="$build_dir/initramfs.cpio.gz"
cat "$zimage" "$dtb" > "$kernel"

compile_recovery_helper() {
    "${compiler[@]}" "${userspace_cppflags[@]}" -O2 -Wall -Wextra -Werror -static \
        "$here/reboot-recovery.c" -o "$build_dir/reboot-recovery"
    qemu-arm "$build_dir/reboot-recovery" --dry-run
}

if [[ "$probe" == lineage14-1 ]]; then
    rom=${V500_M4_ROM:-$here/../../ROMS/lineage-14.1-20261116_215855-UNOFFICIAL-v500.zip}
    python3 "$here/m4-ramdisk.py" "$rom" "$ramdisk"
elif [[ "$probe" == lineage21 ]]; then
    compile_recovery_helper
    qemu-arm "$busybox" sh -n "$m6/stage-init"
    stage_name=${V500_M6_STAGE:-v500-m6}
    case "$stage_name" in
        v500-m6|v500-m6-reference) ;;
        *) printf 'Invalid V500_M6_STAGE: %s\n' "$stage_name" >&2; exit 2 ;;
    esac
    printf '%s\n' "$stage_name" > "$build_dir/v500-m6-stage-name"
    printf '%s\n' "$recovery_seconds" > "$build_dir/v500-m6-recovery-seconds"
    cat > "$build_dir/initramfs.list" <<EOF
dir /v500-m6 0700 0 0
dir /v500-m6/store 0700 0 0
dir /v500-m6/data 0700 0 0
file /v500-m6/busybox $busybox 0755 0 0
file /v500-m6/reboot-recovery $build_dir/reboot-recovery 0755 0 0
file /v500-m6/bootstrap $m6/stage-init 0755 0 0
file /v500-m6/stage-name $build_dir/v500-m6-stage-name 0644 0 0
file /v500-m6/recovery-seconds $build_dir/v500-m6-recovery-seconds 0444 0 0
EOF
    if [[ ${V500_M6_MANUAL_SESSION:-0} == 1 ]]; then
        [[ $stage_name == v500-m6 && ${V500_M6_WALL_CHARGE:-0} == 0 && -f ${port:-/nonexistent}/v500-manual-session.c ]]
        "${compiler[@]}" "${userspace_cppflags[@]}" -O2 -Wall -Wextra -Werror -static \
            "$port/v500-manual-session.c" -o "$build_dir/manual-session"
        printf 'file /v500-m6/manual-session %s/manual-session 0755 0 0\n' "$build_dir" >> "$build_dir/initramfs.list"
    fi
    if [[ ${V500_M6_COMPOSITION_TEST_ADB:-0} == 1 ]]; then
        [[ "$stage_name" == v500-m6 ]]
        printf 'file /v500-m6/composition-adb-root %s/v500-m6-stage-name 0400 0 0\n' "$build_dir" >> "$build_dir/initramfs.list"
    fi
    if [[ ${V500_M6_GPU_BOOT_KEEP_ACTIVE:-0} == 1 ]]; then
        [[ "$stage_name" == v500-m6 && "$bootargs" == *" androidboot.v500_gpu=1"* ]]
        printf 'file /v500-m6/gpu-boot-keep-active %s/v500-m6-stage-name 0400 0 0\n' "$build_dir" >> "$build_dir/initramfs.list"
    fi
    if [[ ${V500_M6_A3XX_BOOT_TRACE:-0} == 1 ]]; then
        [[ "$stage_name" == v500-m6 && "$bootargs" == *" msm.v500_first_hang_trace=1"* ]]
        printf 'file /v500-m6/graphics-boot-trace %s/v500-m6-stage-name 0400 0 0\n' "$build_dir" >> "$build_dir/initramfs.list"
    fi
    cat "$android_ramdisk" > "$ramdisk"
    "$build_dir/usr/gen_init_cpio" -t 0 "$build_dir/initramfs.list" | gzip -n >> "$ramdisk"
    gzip -t "$ramdisk"
else
compile_recovery_helper
qemu-arm "$busybox" sh -n "$here/init"
sed -n '/^cat > \/etc\/profile/,/^EOF/p' "$here/init" | sed '1d;$d' | qemu-arm "$busybox" sh -n

cat > "$build_dir/initramfs.list" <<EOF
dir /bin 0755 0 0
dir /dev 0755 0 0
dir /etc 0755 0 0
dir /proc 0555 0 0
dir /sys 0555 0 0
dir /tmp 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
file /bin/busybox $busybox 0755 0 0
slink /bin/sh busybox 0777 0 0
file /bin/reboot-recovery $build_dir/reboot-recovery 0755 0 0
file /init $here/init 0755 0 0
EOF
if [[ -n ${atomic_module:-} ]]; then
    printf 'file /bin/test_bpf.ko %s 0444 0 0\n' "$atomic_module" >> "$build_dir/initramfs.list"
    printf 'file /bin/test-atomic-add %s/test-atomic-add 0755 0 0\n' "$port" >> "$build_dir/initramfs.list"
fi
if [[ "$probe" == trace && -f ${port:-/nonexistent}/v500-battery-sample ]]; then
    qemu-arm "$busybox" sh -n "$port/v500-battery-sample"
    printf 'file /bin/v500-battery-sample %s/v500-battery-sample 0755 0 0\n' "$port" >> "$build_dir/initramfs.list"
fi
if [[ "$dts" == *-stable ]]; then
    qemu-arm "$busybox" sh -n "$here/v500-load"
    printf 'file /bin/v500-load %s/v500-load 0755 0 0\n' "$here" >> "$build_dir/initramfs.list"
fi
if [[ "$probe" == display ]]; then
    "${compiler[@]}" "${userspace_cppflags[@]}" -O2 -Wall -Wextra -Werror -static \
        "$here/v500-display-test.c" -o "$build_dir/v500-display-test"
    printf 'file /bin/v500-display-test %s/v500-display-test 0755 0 0\n' "$build_dir" >> "$build_dir/initramfs.list"
    qemu-arm "$busybox" sh -n "$here/v500-display-check"
    printf 'file /bin/v500-display-check %s/v500-display-check 0755 0 0\n' "$here" >> "$build_dir/initramfs.list"
fi
if [[ "$probe" == lineage21-probe ]]; then
    "${compiler[@]}" "${userspace_cppflags[@]}" -O2 -Wall -Wextra -Werror -static \
        -I "$source_dir/include/uapi/drm" \
        "$here/v500-lineage21-probe.c" -o "$build_dir/v500-lineage21-probe"
    printf 'file /bin/v500-lineage21-probe %s/v500-lineage21-probe 0755 0 0\n' "$build_dir" >> "$build_dir/initramfs.list"
fi
"$build_dir/usr/gen_init_cpio" -t 0 "$build_dir/initramfs.list" | gzip -n > "$ramdisk"
fi

python3 "$mkbootimg" --kernel "$kernel" --ramdisk "$ramdisk" --cmdline "$bootargs" \
    --base 0x80200000 --pagesize 2048 --ramdisk_offset 0x02000000 \
    --output "$work_image"
python2 "$bump" "$work_image" "$work_image"

python3 - "$work_image" "$reference" "$zimage" "$dtb" "$ramdisk" <<'PY'
from pathlib import Path
from struct import unpack_from
from sys import argv

image, reference, zimage, dtb, ramdisk = (Path(path).read_bytes() for path in argv[1:])
def header(data):
    assert data.startswith(b"ANDROID!")
    return unpack_from("<10I", data, 8)

kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second, second_addr, tags, page, dt_size, os_version = header(image)
old = header(reference)
assert (kernel_addr, ramdisk_addr, second_addr, tags, page, os_version) == (old[1], old[3], old[5], old[6], old[7], old[9])
assert second == dt_size == 0
assert kernel_size == len(zimage) + len(dtb) and ramdisk_size == len(ramdisk)
assert unpack_from("<3I", zimage, 36) == (0x016F2818, 0, len(zimage))
assert dtb[:4] == bytes.fromhex("d00dfeed")
assert image[page:page + kernel_size] == zimage + dtb
ramdisk_offset = page + (kernel_size + page - 1) // page * page
assert image[ramdisk_offset:ramdisk_offset + ramdisk_size] == ramdisk
assert len(image) % page == 16 and image[-16:] == reference[-16:]
assert len(image) <= 24 * 1024 * 1024
print(f"Verified LG boot envelope, appended DTB and initramfs ({len(image)} bytes)")
PY

install -m 0644 "$work_image" "$export_dir/boot-$tag.img"
install -m 0644 "$dtb" "$export_dir/$dts.dtb"
install -m 0644 "$zimage" "$export_dir/zImage-$tag"
install -m 0644 "$build_dir/.config" "$export_dir/config-$tag"
install -m 0644 "$build_dir/System.map" "$export_dir/System.map-$tag"
(
    cd "$export_dir"
    sha256sum "boot-$tag.img" "$dts.dtb" "zImage-$tag" "config-$tag" "System.map-$tag" > SHA256SUMS
)
printf 'Kernel and boot image exported to %s\n' "$export_dir"
