/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_WIFI_CREDENTIAL_H
#define SUNSTONE_WIFI_CREDENTIAL_H
/* The Wi-Fi network credential, from the user's config directory to the radio's supplicant.
 *
 * The user stores a network with tools/wifi-credential.py. A board run that names it (`board.py submit --wifi NAME`)
 * has the runner upload one 256-byte record as its own input, at SS_WIFI_CREDENTIAL_ADDRESS, from memory and never
 * from a file (docs/wifi-credentials.md). The address lies in the radio profile span 0x43000000..0x43100000, whose
 * blocks are EL1 read-write with no EL0 access (platform/a733-smp-cached/memory.c), so no application can read it.
 * The record is not part of the sealed session, the manifest inputs or any result word.
 *
 * Record layout (little-endian), shared with tools/wifi-credential.py (RECORD_*):
 *   0   magic "SSWIFIC1"   8  version (1)   12  reserved (0)
 *   16  flags (bit 0 PMK present, bit 1 password present)   20  ssid_bytes (1..32)
 *   24  password_bytes (1..64 with the password flag, else 0)   28  reserved (0)
 *   32  ssid[32]   64  pmk[32]   96  password[64]   160  zero padding to 256
 */
#include <stddef.h>
#include <stdint.h>

#define SS_WIFI_CREDENTIAL_ADDRESS UINT64_C(0x43004000)
#define SS_WIFI_CREDENTIAL_RECORD_BYTES 256u

/* Return the credential of this session. EL1 only: the supplicant calls it.
 *
 * Returns 0 and sets all five outputs on success. Returns a negative errno when no credential was uploaded for this
 * session (-ENOENT), when the record is malformed (-EINVAL), and after the session ended and the record was wiped
 * (-ENOENT); the outputs are then NULL and 0.
 *
 * *pmk32 points to 32 bytes. *password is NULL, with *password_len 0, when the user stored only the PMK. The pointers
 * address the record itself and stay valid until the session ends, when os_wifi_credential_wipe zeroes it. The
 * supplicant must not copy a value out of it that outlives the session, and must not write through the pointers. */
int ss_wifi_credential(const uint8_t **ssid, size_t *ssid_len, const uint8_t **pmk32, const uint8_t **password,
                       size_t *password_len);

/* Zero the record in RAM with stores the compiler cannot remove, clean its cache lines to DRAM and refuse every later
 * ss_wifi_credential. Called at EL1 when the session ends (ss_finish) and when the board fails (board_fail).
 * Idempotent. */
void os_wifi_credential_wipe(void);

/* 1 once the record has been wiped. */
int os_wifi_credential_wiped(void);

/* EL1 only, from a task that every session end passes: wipe once the application has finished. The wipe
 * executes cache maintenance (dc civac), which EL0 may not do, so ss_finish never calls it; the radio owner
 * polls this every pass, the return supervisor before it lets the return proceed, and board_fail wipes
 * directly. */
void os_wifi_credential_poll(void);

#endif
