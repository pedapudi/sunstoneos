/* Host harness for the EL3 admission of an OS image. tools/host_admission.py links it with the image's own
 * a733_smp_contract_refusal (apps/os/admission.c, or its cpu-return patched copy), os_session_decode_why
 * (apps/os/session.c), the board prefix and contract checks (apps/board/admission.c) and the image's manifest
 * section, all built with the image's compile definitions. Usage:
 *   host_admission SESSION CONTRACT NOW_TICKS
 * SESSION is the exact byte string the runner uploads to A733_BOARD_SESSION_ADDRESS, CONTRACT the contract bytes
 * as the launcher leaves them and NOW_TICKS the counter at the EL3 check. It prints one line:
 *   refusal=0xDDD decode=S stop=0xEEE
 * where DDD is the stop detail of a733_smp_stop(1, DDD) (0 when the board would admit the session), S the
 * session-decode sub-term of include/a733_admission_refusal.h (0 when the decode passed) and EEE the detail the
 * EL3 stop records for the unnamed admission stop (1, 1) after a refused a733_smp_contract_valid (0 when admitted). */
#include "os.h"
#include "boot.h"
#include "a733_admission_refusal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Output through the host's own stdio (tools/host_admission_io.c): the image's include path may carry newlib headers, whose stdout
 * is a reentrancy structure that a host executable does not have. */
void host_err(const char *format, ...);
void host_out(const char *format, ...);
void host_perror(const char *what);
/* The session mapping lives in host_admission_io.c: the system memory-mapping headers conflict with the image's own types. */
void *host_map_session(uintptr_t address, size_t bytes);
/* The first-failure record is not kept on the host: a refused declaration names its reason in the harness output instead. */
void board_fail_note(uint32_t site, uint32_t value, const void *caller) { (void)site; (void)value; (void)caller; }

static size_t slurp(const char *path, void *buffer, size_t capacity)
{
    FILE *f = fopen(path, "rb");
    if (!f) { host_perror(path); exit(2); }
    size_t n = fread(buffer, 1, capacity, f);
    fclose(f);
    return n;
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    /* The decoder reads the session at its fixed board address. */
    void *at = host_map_session(A733_BOARD_SESSION_ADDRESS, 1 << 16);
    if (at != (void *)(uintptr_t)A733_BOARD_SESSION_ADDRESS) { host_err("cannot map the session address\n"); return 3; }
    size_t session = slurp(argv[1], at, 1 << 16);
    struct a733_smp_contract contract;
    memset(&contract, 0, sizeof contract);
    size_t bytes = slurp(argv[2], &contract, sizeof contract);
    if (!session || bytes != sizeof contract) { host_err("short session or contract\n"); return 2; }
    unsigned refusal = a733_smp_contract_refusal(&contract, strtoull(argv[3], NULL, 0));
    struct os_session decoded;
    memset(&decoded, 0, sizeof decoded);
    unsigned why = 0;
    int ok = os_session_decode_why(at, contract.reserved[1], &decoded, &why);
    unsigned stop = 0;
    if (refusal) {
        /* What the board does: the refused check, then the stop that follows it. */
        if (a733_smp_contract_valid(&contract, strtoull(argv[3], NULL, 0))) return 5;
        stop = (unsigned)a733_smp_refusal_detail(1, 1);
    }
    host_out("refusal=0x%x decode=%u stop=0x%x\n", refusal, ok ? 0 : why, stop);
    /* A refused NPU session says which declaration rule refused it (enum npu_declaration_result). */
    if (!ok && why == A733_DECODE_SERVICE)
        host_err("npu declaration result %d, flags %#x\n", (int)os_npu_declaration_admitted(&decoded.npu),
                (unsigned)decoded.npu.flags);
    return 0;
}
