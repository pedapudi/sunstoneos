#include "../apps/bluetooth/bluetooth.h"
#include "a733_storage_digest.h"
#include <assert.h>
#include <stdio.h>
/* The accepted bluetooth-application.c profile, restricted to admission. */
static struct bluetooth_profile profile={.magic=BLUETOOTH_PROFILE_MAGIC,.version=1,.bytes=624,
    .topology=2,.sid_authority=7,.radio_authority=3,.uart_authority=4,.wake_authority=5,
    .wiring_evidence={1},.sid_evidence={2},.fresh_transport_evidence={3},.pm_latch={6,7,0x3f,0},
    .radio={.owner=20,.topology={.identity=2}},.wake={.authority=5,.desired={{1,0,0},{0,0,0}}},
    .uart={.owner=21,.route=A733_UART1_BLUETOOTH,.format={1,8,2,0,1,24000000,1500000,0}},
    .firmware_address={0x43010000,0x43020000,0x43030000,0x43040000},.firmware_bytes={1708,24744,23288,341260},
    .session_owner=22,.runtime_lease_us=10000000,.renew_margin_us=3000000,
    .command_timeout_us=500000,.frame_timeout_us=100000,.binding_timeout_us=1000000};
static int valid(void)
{
    uint8_t digest[32];storage_digest_bytes(&profile,profile.bytes,digest);
    return bluetooth_profile_valid(&profile,digest);
}
int main(void)
{
    assert(valid());profile.pm_latch.known_mask=0;
#ifdef SUNSTONE_A733_OS
    assert(valid());
#else
    assert(!valid());
#endif
    profile.pm_latch.value=1;assert(!valid());profile.pm_latch.value=0;
    profile.pm_latch.known_mask=0x1f;assert(!valid());profile.pm_latch.known_mask=0x3f;
    profile.pm_latch.value=0x40;assert(!valid());profile.pm_latch.value=0;
    profile.pm_latch.identity=0;assert(!valid());profile.pm_latch.identity=6;
    profile.pm_latch.owner=0;assert(!valid());profile.pm_latch.owner=7;
    uint8_t digest[32];storage_digest_bytes(&profile,profile.bytes,digest);digest[0]^=1;
    assert(!bluetooth_profile_valid(&profile,digest));
    assert(valid());
    puts("Bluetooth PM profile: OS/standalone mask policy, malformed records and digest binding passed");
    return 0;
}
