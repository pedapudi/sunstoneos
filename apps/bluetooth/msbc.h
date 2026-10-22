#ifndef SUNSTONE_BLUETOOTH_MSBC_H
#define SUNSTONE_BLUETOOTH_MSBC_H
#include <stdint.h>
/* Called once after native runtime_init and before scheduling. Allocations are
 * retained for this session; the Bluetooth owner is the sole subsequent user. */
int bluetooth_msbc_init(void);
/* Feed one copied transparent byte.1 yields240 bytes of S16LE16k PCM;0 pending;
 * -1 framing/sequence/codec loss. HCI status loss is checked by the caller. */
int bluetooth_msbc_byte(uint8_t,uint64_t,uint8_t[240],uint64_t *);
/* Clears the partial frame, the H2 sequence lock and the discard count; the
 * header search then locks onto the next valid frame. The synthesis history
 * is kept. Called at each activation start. */
void bluetooth_msbc_resync(void);
int bluetooth_msbc_encode(const uint8_t[240],uint8_t[60]);
int bluetooth_msbc_silence(uint8_t[60]);
#endif
