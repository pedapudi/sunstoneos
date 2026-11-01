#ifndef SUNSTONE_I2S_PROBE_H
#define SUNSTONE_I2S_PROBE_H
#include <stdint.h>
#define I2S_PROBE_PERIODS 8u
#define I2S_PROBE_BYTES 1024u
#define I2S_PROBE_FRAMES (I2S_PROBE_BYTES/4)
#define I2S_PROBE_SAMPLES (I2S_PROBE_PERIODS*I2S_PROBE_FRAMES)
#define I2S_PROBE_PAYLOAD UINT64_C(0x42000100)
struct i2s_probe_input { uint32_t mode,periods,rounds,reserved; };
struct i2s_probe_report {
    char result[32];
    uint64_t mode,copies,frames,first,last,rx_packages,tx_packages,interrupts,stage,fault,detail,closed;
};
/* EL3 owns this complete cache-line-multiple snapshot. EL1 reads but never
 * writes its cache lines. State is captured after common monitor setup. */
struct i2s_probe_saved {
    uint32_t active,bus,mbus,security,cpu,mcu,automatic,audio,audio_gate;
    uint32_t group,modifier,priority,configuration;
    uint64_t route;
    uint32_t pad[16];
} __attribute__((aligned(64)));
extern struct i2s_probe_input sunstone_probe_input;
extern struct i2s_probe_saved i2s_probe_saved;
void i2s_probe_irq_enable(void);
void i2s_probe_irq_disable(void);
#endif
