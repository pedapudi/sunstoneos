/* Only the late borrowed-clock admission and new metadata-retain behavior.
 * Reuse setup helpers; do not execute earlier UART protocol scenarios. */
#define main bluetooth_archived_uart_main
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type"
#include "a733-uart-platform.c"
#pragma clang diagnostic pop
#undef main
int main(void)
{
    setup();initialize_fixture(&first,A733_UART1_BLUETOOTH,1,7);ready(&first);
    struct a733_uart *u=a733_uart_platform_controller(&first.binding);assert(u);
    assert(u->deadline==1000);now_us=1500;
    assert(a733_uart_platform_live(&first.binding,3000)==A733_UART_PLATFORM_READY);
    uint64_t observed=0;assert(u->ops.now_us(u->context,&observed)==1&&observed==1500&&u->deadline==3000);
    unsigned before=reads+writes;
    assert(a733_uart_platform_retain(&first.binding)==A733_UART_PLATFORM_RETAINED);
    assert(reads+writes==before&&first.binding.pin_lease.id&&first.binding.clock.id);
    assert(!u->ops.now_us(u->context,&observed)&&reads+writes==before);
    puts("PASS Bluetooth UART: actual live guard authorizes post-bootstrap clock bound; fault retains without device access");
    return 0;
}
