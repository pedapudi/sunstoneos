/* ARM32 proof: the first-stage gate itself, with host-only metadata. */
#include "gate-info.h"
#include "a733_card_fel_gate.h"
.syntax unified
.arch armv8-a
.arm
.section .card_gate,"ax"
.global _start
.global a733_card_fel_gate_image
.type _start,%function
.type a733_card_fel_gate_image,%object
_start:
a733_card_fel_gate_image:
.incbin CARD_FEL_GATE_BINARY
.size a733_card_fel_gate_image,.-a733_card_fel_gate_image
.size _start,.-_start
.section .rodata.profile,"a"
.balign 8
.global a733_card_fel_profile
.type a733_card_fel_profile,%object
a733_card_fel_profile:
.quad 0x314c454644524143,2,88,0x52000,CARD_FEL_GATE_BYTES
.quad A733_CARD_FEL_STAGE,A733_CARD_FEL_BEFORE_ROM
.byte CARD_FEL_SHA256_BYTES
.size a733_card_fel_profile,.-a733_card_fel_profile
