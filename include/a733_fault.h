#ifndef SUNSTONE_A733_FAULT_H
#define SUNSTONE_A733_FAULT_H
/* Retained per-core fault records (docs/fault-capture.md).
 *
 * In the integrated OS image, which defines SUNSTONE_A733_FAULT_PAGE, the EL3
 * fault handler stores a nonzero byte at index core of a733_fault_pending on
 * every entry, and fills the core's slot for an SMC from EL1. The supervising
 * task refuses watchdog renewal while any byte is set, so WDT0 resets the
 * board (docs/reset.md). The next boot copies the valid slots and the
 * previous resident result into a733_fault_previous, which USB0 request byte
 * 1 returns, and clears the slots.
 *
 * The single-core images build the same record on the stack and write it as
 * one log line with a733_fault_line(); they do not use the slots. */
#ifdef __ASSEMBLER__
#define A733_FAULT_U64(x) x
#else
#include <stdint.h>
#define A733_FAULT_U64(x) UINT64_C(x)
#endif

/* Eight 128-byte slots inside the retained .result reservation. EL3 sees the
 * page as Secure Device memory with or without translation, and EL1 has no
 * mapping of it, so every access is an aligned 64-bit load or store. */
#define A733_FAULT_SLOTS_ADDRESS A733_FAULT_U64(0x42001000)
#define A733_FAULT_SLOT_BYTES 128
#define A733_FAULT_CORES 8
/* "A733FLT1" once complete; "A733FLT0" while the capture runs. */
#define A733_FAULT_SLOT_MAGIC A733_FAULT_U64(0x31544c4633333741)
#define A733_FAULT_SLOT_OPEN A733_FAULT_U64(0x30544c4633333741)
/* Word 2: kind in bits 0..7, EL3-recorded vector slot in bits 8..15, core in
 * bits 16..23. A vector fault comes from an EL1 vector entry, which sets
 * x2 = 0x100 | slot before its SMC; any other SMC is a deliberate exit. */
#define A733_FAULT_KIND_VECTOR 1
#define A733_FAULT_KIND_EXIT 2
#define A733_FAULT_SLOT_UNKNOWN 0xff
#ifndef __ASSEMBLER__
#define A733_FAULT_INFO(kind, slot, core) \
    ((uint64_t)(kind) | ((uint64_t)(slot) << 8) | ((uint64_t)(core) << 16))

/* The EL3 capture writes words 2..15, then word 1 = CRC32C (initial value and
 * final XOR 0xffffffff, as crc32cx computes over each word in order) of words
 * 2..15, then word 0. A vector fault also records the four EL1 registers, the
 * address of pxCurrentTCBs[core] and up to 16 bytes of that task's name;
 * these words are zero for an exit. */
struct a733_fault_record {
    uint64_t magic, crc, info, status, detail, counter, epoch;
    uint64_t esr, elr, far, spsr, tcb;
    uint64_t task[2];
    uint64_t reserved[2];
};
_Static_assert(sizeof(struct a733_fault_record) == A733_FAULT_SLOT_BYTES, "fault slot ABI");

/* Previous-boot report, the USB0 response to request byte 1. Header: "FLT1"
 * in bits 0..31, the valid-slot mask in bits 32..39, the incomplete-slot mask
 * (open marker, no complete record) in bits 40..47 and, in bit 48, whether
 * result[] holds the stage-2 loader's copy of the previous resident result.
 * Slots that are not valid read as zero. */
#define A733_FAULT_REPORT_MAGIC UINT64_C(0x31544c46)
#define A733_FAULT_REPORT_RESULT (UINT64_C(1) << 48)
struct a733_fault_report {
    uint64_t header;
    uint64_t result[16];
    struct a733_fault_record slot[A733_FAULT_CORES];
};
_Static_assert(sizeof(struct a733_fault_report) == 1160, "fault report ABI");

/* The stage-2 loader's report (boot/a733/stage2/loader.c): magic word 0,
 * status word 3, and the previous resident result in words 24..39 when the
 * status is ENTERED. */
#define A733_FAULT_STAGE2_REPORT UINT64_C(0x47008200)
#define A733_FAULT_STAGE2_MAGIC UINT64_C(0x4153373353544732)
#define A733_FAULT_STAGE2_ENTERED 6u
#define A733_FAULT_STAGE2_RESULT_WORD 24u

#ifdef SUNSTONE_A733_FAULT_PAGE
/* EL1 .bss. Byte c is nonzero once core c entered the EL3 fault handler. */
extern uint64_t a733_fault_pending;
extern struct a733_fault_report a733_fault_previous;
/* Core 0 at EL3 with translation off, once per boot, before
 * a733_cached_prepare publishes image RAM. */
void a733_fault_boot(void);
#endif

/* One log line of a record for the single-core images, at most
 * A733_FAULT_LINE_BYTES including the terminating NUL. checks/fault-decode.py
 * formats the same line. Returns its length. */
#define A733_FAULT_LINE_BYTES 160u
static inline unsigned a733_fault_line_text(char *out, unsigned n, const char *text)
{
    while (*text) out[n++] = *text++;
    return n;
}
static inline unsigned a733_fault_line_hex(char *out, unsigned n, const char *label, uint64_t value)
{
    n = a733_fault_line_text(out, n, label);
    n = a733_fault_line_text(out, n, "0x");
    for (unsigned i = 0; i < 16; ++i) out[n++] = "0123456789abcdef"[(value >> (60 - 4 * i)) & 15];
    return n;
}
static inline unsigned a733_fault_line(const struct a733_fault_record *r, char out[A733_FAULT_LINE_BYTES])
{
    unsigned n = a733_fault_line_text(out, 0, "FAIL exception core=");
    out[n++] = (char)('0' + ((r->info >> 16) & 7));
    n = a733_fault_line_hex(out, n, " ESR=", r->esr);
    n = a733_fault_line_hex(out, n, " ELR=", r->elr);
    n = a733_fault_line_hex(out, n, " FAR=", r->far);
    n = a733_fault_line_hex(out, n, " SPSR=", r->spsr);
    n = a733_fault_line_text(out, n, " task=");
    /* Printable ASCII up to the first NUL; anything else becomes '?'. */
    for (unsigned i = 0; i < 16; ++i) {
        char c = (char)(r->task[i / 8] >> (8 * (i % 8)));
        if (!c) break;
        out[n++] = c >= 0x20 && c < 0x7f ? c : '?';
    }
    out[n++] = '\n';
    out[n] = 0;
    return n;
}
#endif /* __ASSEMBLER__ */
#endif
