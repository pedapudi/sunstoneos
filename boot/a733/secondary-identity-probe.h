#ifndef A733_SECONDARY_IDENTITY_PROBE_H
#define A733_SECONDARY_IDENTITY_PROBE_H
#define IDENTITY_REPORT 0x42018100
#define IDENTITY_SLOTS 0x42018200
#define IDENTITY_REPORT_BYTES 0x900
#ifndef __ASSEMBLER__
#include <stdint.h>
#include <stddef.h>
struct identity_header {
    uint64_t magic, version, bytes, epoch, deadline, completed, state, core;
    uint64_t reserved[24];
};
struct identity_slot {
    uint64_t epoch, core, midr, mpidr, sctlr3, sctlr1, tcr1;
    uint64_t clusteridr, clustercfr, clidr, mmfr0;
    uint64_t power_before, power_after, target_control, target_mode;
    uint64_t ready, request, ack;
    uint64_t architecture, entry_low, entry_high, control, mode, off, restored;
    uint64_t owner_state, report_state, reserved[5];
};
_Static_assert(sizeof(struct identity_header)==256,"header ABI");
_Static_assert(sizeof(struct identity_slot)==256,"per-core ABI");
_Static_assert(offsetof(struct identity_slot,ready)==120,"target-ready ABI");
_Static_assert(offsetof(struct identity_slot,ack)==136,"target-OFF acknowledgement ABI");
#endif
#endif
