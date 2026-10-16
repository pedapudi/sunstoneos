#ifndef SUNSTONE_USB_SERVICE_H
#define SUNSTONE_USB_SERVICE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void usb_runtime_start(unsigned priority);
uint64_t usb_runtime_last_progress(void);
// Called only by the USB task. Input and output bodies are at most 2048 bytes.
bool usb_application_dispatch(uint32_t command, const uint8_t *input,
                              uint32_t size, uint8_t *output, unsigned *length);
#ifdef __cplusplus
}
#endif
#endif
