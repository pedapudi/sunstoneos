#include "../storage-read-single-core/boot.h"
#undef RESULT_BASE
extern unsigned char __crypto_platform_log[];
#define RESULT_BASE ((uintptr_t)__crypto_platform_log)
