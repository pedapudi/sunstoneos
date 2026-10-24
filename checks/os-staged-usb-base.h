/* Hardware boundary for os-staged-recorded.c. The production USB exchange
 * supplies BOARD_USB0_STOP, including conditional Type-C revocation. */
static _Noreturn void stop(void){BOARD_USB0_STOP();finish(2);}
static void usb0_service_start(void){if(fixture_start_failed)stop();}
static _Noreturn void usb0_service_run(void (*pass)(void)){(void)pass;finish(1);}
