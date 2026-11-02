#ifndef A733_ADMISSION_REFUSAL_H
#define A733_ADMISSION_REFUSAL_H
#include <stdint.h>
/* The named stop of EL3 admission. A refused contract or session stops the board with
 * a733_smp_stop(1, detail), and the detail names the failing term:
 *
 *   detail = 0x100 | term << 4 | sub        (0x100 to 0x1ff, kept whole by the 12-bit breadcrumb detail)
 *
 * Detail 1 is the unnamed stop of earlier images; detail 2 is the watchdog readback stop.
 * tools/a733-crumbs.py decodes the same table (TERMS and SUBTERMS there); checks/admission-refusal.py
 * requires the two to agree. The host pre-flight (tools/host_admission.py) prints the same names. */
#define A733_REFUSAL(term, sub) (0x100u | (unsigned)(term) << 4 | (unsigned)(sub))
enum a733_refusal_term {
    A733_REFUSAL_BOOT_STATE = 1,      /* not EL3 on core 0, or SCTLR_EL3 has a rejected bit */
    A733_REFUSAL_MAGIC_VERSION = 2,   /* contract magic, version or size */
    A733_REFUSAL_BUILD_ID = 3,        /* the contract's build ID is not this image's */
    A733_REFUSAL_DEVICE_MEMORY = 4,   /* the device-memory handoff words */
    A733_REFUSAL_TIMING = 5,          /* armed_at, startup, diagnostic and expiry deadlines, watchdog fields */
    A733_REFUSAL_SESSION_DECODE = 6,  /* os_session_decode (apps/os/session.c); the sub-term says which rule */
    A733_REFUSAL_SESSION_BIND = 7,    /* the board prefix, its digest, or the contract-to-session binding */
    A733_REFUSAL_STALE_RESULT = 8,    /* the staged contract changed during the copy, or a result exists */
    A733_REFUSAL_CONTRACT_FIELD = 9,  /* epoch, generation, inverse generation or lease */
};
enum a733_refusal_sub_timing {
    A733_TIMING_ARMED = 1, A733_TIMING_STARTUP = 2, A733_TIMING_DIAGNOSTIC = 3, A733_TIMING_EXPIRY = 4,
    A733_TIMING_WATCHDOG_FIELDS = 5,
};
enum a733_refusal_sub_decode {
    A733_DECODE_FRAMING = 1,          /* session size, header magic, version, flags or device bits */
    A733_DECODE_MANIFEST = 2,         /* manifest section, its digest, or the granted devices against it */
    A733_DECODE_DEVICES = 3,          /* a device combination this image does not support */
    A733_DECODE_WORKLOAD = 4,         /* the board prefix's workload against the granted devices */
    A733_DECODE_EXTENT = 5,           /* a service extent that is missing, misplaced or of the wrong size */
    A733_DECODE_INPUT = 6,            /* an input fact against its manifest region */
    A733_DECODE_RETURN_RESERVE = 7,   /* manifest return_reserve_us is not shorter than the session lifetime */
    A733_DECODE_RUN_DEADLINE = 8,     /* manifest run_us ends later than the lifetime minus return_reserve_us */
    A733_DECODE_SERVICE = 9,          /* os_session_valid: a service record field */
    A733_DECODE_AUDIO_FRAMES = 10,    /* the capture block against the manifest's audio bounds */
};
enum a733_refusal_sub_magic { A733_MAGIC_MAGIC = 1, A733_MAGIC_VERSION = 2, A733_MAGIC_SIZE = 3 };
enum a733_refusal_sub_field { A733_FIELD_IDENTITY = 1 };    /* epoch, generation, inverse generation or lease */
enum a733_refusal_sub_bind {
    A733_BIND_DIGEST = 1, A733_BIND_BOARD_PREFIX = 2, A733_BIND_CONTRACT = 3,
};
enum a733_refusal_sub_stale { A733_STALE_SOURCE = 1, A733_STALE_RESULT = 2 };
struct a733_smp_contract;
/* Zero when the staged contract is admitted, otherwise a named detail: the OS admission (apps/os/admission.c). */
unsigned a733_smp_contract_refusal(const struct a733_smp_contract *, uint64_t now);
/* The detail of a733_smp_stop(status, detail): the unnamed admission stop (1, 1) becomes the refusal of the last
 * admission check on this boot; every other stop keeps its detail. */
uint64_t a733_smp_refusal_detail(unsigned status, uint64_t detail);
#endif
