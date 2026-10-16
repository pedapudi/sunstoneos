#ifndef SUNSTONE_MIMUS_SERVICE_H
#define SUNSTONE_MIMUS_SERVICE_H
#include <span>
#include <cstdint>
void mimus_service_init();
void mimus_service_wait(std::span<const uint8_t> inputs[3]);
void mimus_service_finish(bool success);
#endif
