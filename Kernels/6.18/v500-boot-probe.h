/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __ASM_ARM_V500_BOOT_PROBE_H
#define __ASM_ARM_V500_BOOT_PROBE_H

#if V500_BOOT_PROBE

/*
 * APQ8064 global TMR0 and IMEM sequence from the legacy V500 restart path
 * with CONFIG_LGE_BITE_RESET. Used with the MMU and data cache off.
 * Preserve r0-r2, r4-r9 and lr for the normal kernel handoff.
 */
	.macro	v500_arm_recovery_watchdog, ticks
#ifdef V500_RECOVERY_TIMEOUT
	.if	(\ticks) == (45 << 15)
	v500_recovery_ticks = (V500_RECOVERY_TIMEOUT << 15)
	.else
	v500_recovery_ticks = (\ticks)
	.endif
#else
	v500_recovery_ticks = (\ticks)
#endif
	movw	r10, #0xf000
	movt	r10, #0x2a03		@ APQ8064 IMEM
	mov	r11, #0
	str	r11, [r10]		@ Clear both download mode words
	str	r11, [r10, #4]
	movw	ip, #0x5502
	movt	ip, #0x7766		@ Bootloader recovery reason
	str	ip, [r10, #0x65c]
	dsb	sy

	movw	r10, #0xa000
	movt	r10, #0x0208		@ APQ8064 global TMR0, not local TMR
	str	r11, [r10, #0x40]	@ Stop watchdog
	mov	r11, #1
	str	r11, [r10, #0x38]	@ Reset counter
	movw	ip, #(v500_recovery_ticks & 0xffff)
	movt	ip, #(v500_recovery_ticks >> 16)
	add	r3, ip, ip, lsl #2	@ Bark after bite
	str	r3, [r10, #0x4c]	@ Bark
	str	ip, [r10, #0x5c]	@ Bite
	str	r11, [r10, #0x40]	@ Enable watchdog
	dsb	sy
	.endm

	.macro	v500_watchdog_recovery, checkpoint
	.type	\checkpoint, %function
\checkpoint:
	v500_ram_console_mark V500_BOOT_PROBE
	v500_arm_recovery_watchdog 0x31f3
1:	wfi
	b	1b
	.size	\checkpoint, . - \checkpoint
	.endm

/*
 * Write "V500 BOOT N\n" in the legacy non-ECC persistent RAM console format.
 * TWRP exposes this buffer as /proc/last_kmsg after a warm reset.
 * Mainline trace builds hand the same buffer to the ramoops console.
 * Clobbers only r3 and ip while the MMU and data cache are off.
 */
	.macro	v500_ram_console_mark, stage
	movw	ip, #0
	movt	ip, #0x8870		@ Legacy V500 ram_console base
	movw	r3, #0x3556
	movt	r3, #0x3030		@ "V500"
	str	r3, [ip, #12]
	movw	r3, #0x4220
	movt	r3, #0x4f4f		@ " BOO"
	str	r3, [ip, #16]
	movw	r3, #0x2054
	movt	r3, #(0x0a30 + \stage)	@ "T N\n"
	str	r3, [ip, #20]
	mov	r3, #12
	str	r3, [ip, #4]		@ Ring start index
	str	r3, [ip, #8]		@ Valid bytes
	movw	r3, #0x4244
	movt	r3, #0x4347		@ DBGC signature
	str	r3, [ip]
	dsb	sy
	.endm
#endif

#endif
