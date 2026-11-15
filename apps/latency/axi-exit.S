/* SPDX-License-Identifier: GPL-2.0-only */
/* The success terminal's EL1 side in the stage 3a cap image (apps/latency/axi-privilege.c, cmake/a733-os.cmake).
 * Each core's terminal task runs a733_qualification_worker_done at EL0 and ends with SVC #0x735; the EL1 vector
 * branches here with interrupts masked, and x0 and x1 restored. Every clean return and every failure return the OS
 * claims takes this path. Before passing on, the cap's enable word, +0x30, is written 0 on both AXI2HIF bridges: two
 * plain stores between DSBs, through x16 and x17, which the terminal does not read. */
.section .text.latency_axi_exit,"ax"
.balign 4
.global __wrap_a733_qualification_terminal_privileged
__wrap_a733_qualification_terminal_privileged:
    movz w16, #0x0030
    movk w16, #0x0a03, lsl #16
    movz w17, #0x0030
    movk w17, #0x0a04, lsl #16
    dsb sy
    str wzr, [x16]
    str wzr, [x17]
    dsb sy
    b __real_a733_qualification_terminal_privileged
