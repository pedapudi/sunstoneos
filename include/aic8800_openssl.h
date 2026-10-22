#ifndef SUNSTONE_AIC8800_OPENSSL_H
#define SUNSTONE_AIC8800_OPENSSL_H
#include <stddef.h>
/* One-time setup of the pinned OpenSSL library for station SAE. Call only
 * from the radio owner task inside a supplicant OS entry; the library is built
 * without threads. Its allocations come from the FreeRTOS heap and are wiped
 * when freed. The random generator is seeded only from the entered
 * supplicant's entropy callback; outside an entry every seed request fails.
 * Returns 1 when ready; a failure is permanent for this image lifetime. */
int aic8800_openssl_init(void);
/* Heap bytes currently held and the largest amount held at once. */
size_t aic8800_openssl_heap_bytes(size_t *peak);
#endif
