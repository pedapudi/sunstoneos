/* Host test of apps/os/wifi_credential.c: the record receiver of the OS image. The record buffer is a host array;
 * the real one is at SS_WIFI_CREDENTIAL_ADDRESS. checks/wifi-credential.py supplies a fake credential's record and
 * the structure it must decode to. Nothing here prints a byte of either. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile uint8_t buffer[256];
#define SS_WIFI_CREDENTIAL_BASE (buffer)
#include "../apps/os/wifi_credential.c"

static void load(const uint8_t *record) { for (unsigned i = 0; i < 256; ++i) buffer[i] = record[i]; }
static int all_zero(const volatile uint8_t *p, size_t n) { for (size_t i = 0; i < n; ++i) if (p[i]) return 0; return 1; }
static void read_file(const char *path, void *out, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f || fread(out, 1, n, f) != n) { puts("FAIL unreadable fixture"); exit(2); }
    fclose(f);
}
#define CHECK(cond, text) do { if (!(cond)) { puts("FAIL " text); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    uint8_t record[256], ssid[32], pmk[32], password[64];
    read_file(argv[1], record, sizeof record);
    read_file(argv[2], ssid, 16);        /* the SSID, 16 bytes */
    read_file(argv[3], pmk, sizeof pmk);
    read_file(argv[4], password, 18);    /* the password, 18 bytes */
    const uint8_t *s, *k, *p;
    size_t sl, pl;
    #define GET() ss_wifi_credential(&s, &sl, &k, &p, &pl)
    #define CLEAR() (!s && !k && !p && !sl && !pl)

    /* No record: -ENOENT and empty outputs. */
    s = (void *)1; pl = 9;
    CHECK(GET() == -2 && CLEAR(), "absent record accepted");

    /* The runner's record decodes; the pointers address the record itself. */
    load(record);
    CHECK(GET() == 0, "record refused");
    CHECK(sl == 16 && !memcmp(s, ssid, 16) && !memcmp(k, pmk, 32) && pl == 18 && !memcmp(p, password, 18), "wrong values");
    CHECK(s == (const uint8_t *)buffer + 32 && k == (const uint8_t *)buffer + 64 && p == (const uint8_t *)buffer + 96, "pointers");
    CHECK(ss_wifi_credential(NULL, &sl, &k, &p, &pl) == -22, "null output accepted");

    /* A PMK-only record has a NULL password. */
    uint8_t only[256];
    memcpy(only, record, sizeof only);
    only[16] = 1; only[24] = 0;
    memset(only + 96, 0, 64);
    load(only);
    CHECK(GET() == 0 && p == NULL && pl == 0 && k == (const uint8_t *)buffer + 64, "PMK-only record");

    /* Malformed records are refused with -EINVAL and empty outputs. */
    static const struct { unsigned offset; uint8_t value; const char *what; } bad[] = {
        {0, 'X', "magic"}, {8, 2, "version"}, {12, 1, "reserved word"}, {16, 0, "missing PMK flag"}, {16, 5, "unknown flag"},
        {20, 0, "empty SSID"}, {20, 33, "long SSID"}, {24, 1, "password length changed"},
        {28, 1, "body reserved word"}, {200, 1, "nonzero padding"}, {60, 1, "SSID tail"}};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        uint8_t mutated[256];
        memcpy(mutated, record, sizeof mutated);
        if (mutated[bad[i].offset] == bad[i].value) mutated[bad[i].offset] ^= 0x80;
        else mutated[bad[i].offset] = bad[i].value;
        load(mutated);
        if (GET() != -22 || !CLEAR()) {
            printf("FAIL malformed record accepted: %s\n", bad[i].what);
            return 1;
        }
    }

    /* The wipe zeroes the record and refuses every later read, even of a rewritten record. */
    load(record);
    CHECK(GET() == 0, "reload failed");
    os_wifi_credential_wipe();
    CHECK(all_zero(buffer, sizeof buffer), "record not wiped");
    load(record);
    CHECK(GET() == -2 && CLEAR(), "read after wipe accepted");
    os_wifi_credential_wipe();
    CHECK(all_zero(buffer, sizeof buffer), "second wipe left bytes");
    puts("PASS");
    return 0;
}
