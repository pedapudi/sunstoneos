#ifndef MIMUS_SS_ADAPTER_H
#define MIMUS_SS_ADAPTER_H
#include "sunstone_app.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Main-task transcript storage is immutable between READY and a delivered
 * acknowledgement. Only the responder reads it and publishes acknowledgements. */
unsigned mimus_started(void);
unsigned mimus_generation(void);
int mimus_result_copy(uint32_t, size_t, uint8_t *, size_t *, uint32_t *);
int mimus_result_ack(unsigned);
void mimus_storage_completed(unsigned);
int mimus_storage_ready(unsigned);
int mimus_sector_copy(uint8_t [512]);
int mimus_host_start(void);
int mimus_trigger_step(void);
int mimus_trigger_copy(uint8_t [128]);
int mimus_trigger_rearm(void);
void mimus_init(void);
void mimus_main(const struct ss_grant *);
#ifdef __cplusplus
}
#endif
#endif
