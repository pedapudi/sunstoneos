#include "sunstone_app.h"
/* The C library's console output ends the session: the application prints nothing. Exception code can reach this
 * function through the C library's stream callbacks (checks/irq-general-regs.py), so it stays out of main.c, which uses
 * FP, and builds with -mgeneral-regs-only (cmake/application.cmake). */
void platform_puts(const char *text){(void)text;ss_fail(0x8d0);}
