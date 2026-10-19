#ifndef SUNSTONE_BLUETOOTH_MSBC_H
#define SUNSTONE_BLUETOOTH_MSBC_H
#include <stdint.h>
/* Called once after native runtime_init and before scheduling. Allocations are
 * retained for this session; the Bluetooth owner is the sole subsequent user. */
int bluetooth_msbc_init(void);
/* Feed one copied transparent byte.1 yields240 bytes of S16LE16k PCM;0 pending;
 * -1 framing/sequence/codec loss. HCI status loss is checked by the caller. */
int bluetooth_msbc_byte(uint8_t,uint64_t,uint8_t[240],uint64_t *);
int bluetooth_msbc_silence(uint8_t[60]);
#endif
