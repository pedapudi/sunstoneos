/* Model platform services; panic and exit end the modeled run. */
#ifndef MODEL_PLATFORM_H
#define MODEL_PLATFORM_H
#include <stdint.h>
void platform_init(void);
void platform_puts(const char *text);
_Noreturn void platform_exit(unsigned status);
#endif
