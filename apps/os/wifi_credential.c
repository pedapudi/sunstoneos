/* The Wi-Fi credential receiver of the OS image (include/sunstone_wifi_credential.h, docs/wifi-credentials.md).
 *
 * The runner writes one 256-byte record into the EL1-only radio profile span before launch. This file validates it
 * once and gives the supplicant pointers into it, and wipes it at the end of the session and on a fault. The value
 * never goes into a result word, a failure note or a diagnostic: nothing in this file formats or records a byte of it. */
#include "sunstone_wifi_credential.h"
#include <stdatomic.h>
#include <string.h>

#ifndef SS_WIFI_CREDENTIAL_BASE
#define SS_WIFI_CREDENTIAL_BASE ((volatile uint8_t *)(uintptr_t)SS_WIFI_CREDENTIAL_ADDRESS)
#endif
#define RECORD_MAGIC "SSWIFIC1"
#define RECORD_VERSION 1u
#define FLAG_PMK 1u
#define FLAG_PASSWORD 2u
#define SSID_OFFSET 32u
#define PMK_OFFSET 64u
#define PASSWORD_OFFSET 96u
#define E_NOENT 2
#define E_INVAL 22

static atomic_uint wiped;

/* Volatile stores and a memory barrier: the compiler cannot treat the zeroing as dead because the buffer is not read
 * afterwards, and cannot reorder it past the barrier. */
static void zero(volatile uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) p[i] = 0;
    __asm__ volatile("" ::: "memory");
}

static uint32_t word(unsigned offset)
{
    uint32_t v = 0;
    for (unsigned i = 4; i--;) v = v << 8 | SS_WIFI_CREDENTIAL_BASE[offset + i];
    return v;
}

/* Every byte of [from, to) is zero. */
static int zeros(unsigned from, unsigned to)
{
    uint8_t any = 0;
    for (unsigned i = from; i < to; ++i) any |= SS_WIFI_CREDENTIAL_BASE[i];
    return !any;
}

static int valid(uint32_t flags, uint32_t ssid_bytes, uint32_t password_bytes)
{
    for (unsigned i = 0; i < 8; ++i)
        if (SS_WIFI_CREDENTIAL_BASE[i] != (uint8_t)RECORD_MAGIC[i]) return 0;
    return word(8) == RECORD_VERSION && !word(12) && (flags & FLAG_PMK) && !(flags & ~(FLAG_PMK | FLAG_PASSWORD)) &&
        ssid_bytes >= 1 && ssid_bytes <= 32 && !word(28) &&
        ((flags & FLAG_PASSWORD) ? password_bytes >= 1 && password_bytes <= 64 : password_bytes == 0) &&
        zeros(SSID_OFFSET + ssid_bytes, PMK_OFFSET) && zeros(PASSWORD_OFFSET + password_bytes, 160) &&
        zeros(160, SS_WIFI_CREDENTIAL_RECORD_BYTES);
}

int ss_wifi_credential(const uint8_t **ssid, size_t *ssid_len, const uint8_t **pmk32, const uint8_t **password,
                       size_t *password_len)
{
    if (!ssid || !ssid_len || !pmk32 || !password || !password_len) return -E_INVAL;
    *ssid = *pmk32 = *password = NULL;
    *ssid_len = *password_len = 0;
    if (atomic_load_explicit(&wiped, memory_order_acquire)) return -E_NOENT;
    uint32_t flags = word(16), ssid_bytes = word(20), password_bytes = word(24);
    if (zeros(0, SS_WIFI_CREDENTIAL_RECORD_BYTES)) return -E_NOENT;
    if (!valid(flags, ssid_bytes, password_bytes)) return -E_INVAL;
    volatile uint8_t *base = SS_WIFI_CREDENTIAL_BASE;
    *ssid = (const uint8_t *)(uintptr_t)(base + SSID_OFFSET);
    *ssid_len = ssid_bytes;
    *pmk32 = (const uint8_t *)(uintptr_t)(base + PMK_OFFSET);
    if (flags & FLAG_PASSWORD) {
        *password = (const uint8_t *)(uintptr_t)(base + PASSWORD_OFFSET);
        *password_len = password_bytes;
    }
    return 0;
}

void os_wifi_credential_wipe(void)
{
    atomic_store_explicit(&wiped, 1, memory_order_release);
    zero(SS_WIFI_CREDENTIAL_BASE, SS_WIFI_CREDENTIAL_RECORD_BYTES);
#if defined(__aarch64__)
    /* The block is write-back: clean the lines so that DRAM holds zeros, not just the cache. */
    for (uintptr_t a = (uintptr_t)SS_WIFI_CREDENTIAL_BASE & ~(uintptr_t)63;
         a < (uintptr_t)SS_WIFI_CREDENTIAL_BASE + SS_WIFI_CREDENTIAL_RECORD_BYTES; a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
#endif
}
