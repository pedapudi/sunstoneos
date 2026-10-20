#ifndef MIMUS_BOARD_KEYBOARD_H
#define MIMUS_BOARD_KEYBOARD_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Only copied CPU bytes cross this one-producer/one-consumer mailbox. */
struct mimus_keyboard_report {
    uint64_t generation,sequence;
    uint8_t address,speed,interface,protocol,endpoint,subclass,mode,bytes;
    uint8_t data[8];
};
int mimus_keyboard_empty(void);
/* Consumer only, after delivery-owned evidence has been acknowledged. */
int mimus_keyboard_rearm(void);
int mimus_keyboard_offer(const struct mimus_keyboard_report *);
int mimus_keyboard_step(void);
int mimus_keyboard_copy(uint8_t out[128]);
#ifdef __cplusplus
}
#endif
#endif
