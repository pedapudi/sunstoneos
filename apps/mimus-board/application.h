#ifndef MIMUS_BOARD_APP_H
#define MIMUS_BOARD_APP_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
int mimus_board_live(void);
int mimus_board_session_live(void);
void mimus_board_host_observed(int);
int mimus_board_export_live(void);
void mimus_board_fail(void);
int mimus_board_keyboard(void);
int mimus_board_trigger_step(void);
int mimus_board_trigger_copy(uint8_t [128]);
int mimus_board_start(void);
unsigned mimus_board_started(void);
unsigned mimus_board_generation(void);
int mimus_board_ack(unsigned);
int mimus_board_rearm(void);
void mimus_board_storage_completed(unsigned);
int mimus_board_copy(uint32_t,size_t,uint8_t *,size_t *,uint32_t *);
#ifdef __cplusplus
}
#endif
#endif
