/* SPDX-License-Identifier: Apache-2.0 */
.syntax unified
.thumb

.section .dfu_vectors, "a", %progbits
.global dfu_vectors
dfu_vectors:
    .word __stack_top
    .word dfu_reset
    .rept 13
    .word dfu_fault
    .endr
    .word SysTick_Handler

.section .text.dfu_reset, "ax", %progbits
.global dfu_reset
.global entry
.thumb_func
dfu_reset:
    cpsid i
    movs r0, #0
    msr control, r0
    ldr r1, =__stack_limit
    msr msplim, r1
    isb
    ldr r1, =__bss_start__
    ldr r2, =__bss_end__
1:  cmp r1, r2
    bhs 2f
    str r0, [r1], #4
    b 1b
2:  ldr r0, =__flash_ops_load
    ldr r1, =__flash_ops_start
    ldr r2, =__flash_ops_end
3:  cmp r1, r2
    bhs 4f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 3b
4:  dsb
    isb
    bl dfu_main
    b dfu_fault

.thumb_set entry, dfu_reset
