#include "FreeRTOS.h"

// The prebuilt diagnostic handler reads a global newlib task pointer.
// Termination on this exception-free runtime needs no allocation or stdio.
namespace __gnu_cxx {
[[noreturn]] void __verbose_terminate_handler() {
    platform_panic("C++ termination", 0);
}
}
