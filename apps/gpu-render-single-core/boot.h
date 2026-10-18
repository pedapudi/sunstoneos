#include "../storage-read-single-core/boot.h"
#undef RESULT_BASE
extern unsigned char __gpu_platform_log[];
#define RESULT_BASE ((uintptr_t)__gpu_platform_log)
