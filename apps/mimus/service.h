#ifndef SUNSTONE_MIMUS_SERVICE_H
#define SUNSTONE_MIMUS_SERVICE_H
#include <span>
#include <cstdint>
void mimus_service_init();
// Returns true for paced utterance replay, false for whole-recording recognition.
bool mimus_service_wait(std::span<const uint8_t> inputs[3]);
void mimus_service_finish(bool success);
#endif
