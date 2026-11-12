/* ARM32 proof: the first-stage gate itself, with host-only metadata.
 * CARD_FEL_PROOF_ENTRY selects the entry state the runner sets: 1, the FEL
 * request; 2, no request with the cold-boot marker set (a warm reset). */
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
.global a733_card_fel_gate_watchdog
.type a733_card_fel_gate_watchdog,%object
a733_card_fel_gate_watchdog:          /* The gate's WDT0 policy word for this proof. */
.word A733_CARD_FEL_PROOF_WATCHDOG
.size a733_card_fel_gate_watchdog,4
.size _start,.-_start
.section .rodata.profile,"a"
.balign 8
.global a733_card_fel_profile
.type a733_card_fel_profile,%object
a733_card_fel_profile:
.quad 0x314c454644524143,4,104,0x52000,CARD_FEL_GATE_BYTES
.quad A733_CARD_FEL_STAGE,A733_CARD_FEL_BEFORE_ROM,A733_CARD_FEL_PROOF_WATCHDOG,CARD_FEL_PROOF_ENTRY
.byte CARD_FEL_SHA256_BYTES
.size a733_card_fel_profile,.-a733_card_fel_profile
