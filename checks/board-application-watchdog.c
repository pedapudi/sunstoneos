/* Exercise the actual fixed composition's supervisor with a modeled hardware
 * reload boundary. No scheduler heartbeat may earn a reload. */
#include "../apps/board/main.c"
static unsigned mode,delays,reloads,revocations;
volatile unsigned board_check_result;
static void finish(unsigned result)
{board_check_result=result;__asm__ volatile("brk #0x733");for(;;);}
uint64_t a733_smp_counter(void){return 2400;}
int board_radio_watchdog(struct board_radio *r,uint64_t *progress,uint64_t *deadline)
{if(r!=&radio)finish(10);*progress=mode?1:0;*deadline=mode==2?90:6000;return 1;}
int a733_watchdog_hw_reload(const struct a733_watchdog_window *window,
    struct a733_counter_clock *clock,struct a733_platform_locks *locks,void *task,
    atomic_uint *state,uint64_t deadline,uint64_t *before,uint64_t *after)
{
    if(mode!=1||window!=&board.window||clock!=&providers.shared.counter||
       locks!=&providers.shared.locks||task!=board.handles[7]||state!=&board.watchdog_state||
       deadline!=5000||++reloads!=1)finish(11);
    *before=100;*after=101;return 1;
}
void a733_typec_revoke(struct a733_typec *p)
{if(p!=&board.typec.provider)finish(12);++revocations;}
void vTaskDelay(TickType_t ticks)
{
    if(ticks!=pdMS_TO_TICKS(20))finish(13);
    if(++delays<3)return;
    if(mode==1){
        if(reloads!=1||revocations||atomic_load(&board.live_until)!=board.session.deadline_us)finish(14);
    }else if(mode==2){
        if(reloads||revocations!=1||atomic_load(&board.watchdog_state)!=5)finish(15);
    }else if(reloads||revocations||atomic_load(&board.live_until)!=5000)finish(16);
    finish(1);
}
void board_check_run(unsigned which)
{
    mode=which;board.session.deadline_us=20000;board.session.minimum_reload_us=50000;
    board.session.guard_us=1000;board.handles[7]=(void *)0x1234;
    atomic_init(&board.watchdog_state,1);atomic_init(&board.live_until,5000);
    supervise(0);
}
