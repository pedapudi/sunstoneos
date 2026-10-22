#include "../storage-read-single-core/boot.h"
#undef RESULT_BASE
/* Each image's linker script names the platform log after its application;
 * images derived from this one select their symbol with this definition. */
#ifndef SUNSTONE_SINGLE_CORE_PLATFORM_LOG
#define SUNSTONE_SINGLE_CORE_PLATFORM_LOG __board_platform_log
#endif
extern unsigned char SUNSTONE_SINGLE_CORE_PLATFORM_LOG[];
#define RESULT_BASE ((uintptr_t)SUNSTONE_SINGLE_CORE_PLATFORM_LOG)
