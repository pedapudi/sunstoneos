.syntax unified
.arch armv8-a
.arm
.section .entry, "ax"
.global restore_entry
restore_entry:
    b restore_admit
    b restore_write
    b restore_check
