/* Execute the OS thermal owner's real step, recording and publication paths.
 * Board authority, RTOS identity, THS completion and policy values are modeled;
 * no MMIO, clocks, voltage or physical sensor behavior is qualified here. */
#include "../apps/os/thermal.c"
struct board_application board;
struct board_provider providers;
static uint64_t model_now,read_delay;
static unsigned read_count,critical,failed;
uint64_t thermal_cadence_result[8];
int board_live(void *unused){(void)unused;return 1;}
int os_application_deadline(void){return 1;}
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (TaskHandle_t)1;}
uint64_t a733_smp_counter(void){return model_now*24;}
int a733_counter_clock_now(void *unused,uint64_t *out){(void)unused;*out=model_now;return 1;}
enum a733_thermal_result a733_thermal_poll(struct a733_thermal *driver)
{(void)driver;return A733_THERMAL_READY;}
enum a733_thermal_result a733_thermal_read(struct a733_thermal *driver,uint64_t until,struct a733_thermal_sample *sample)
{(void)driver;(void)sample;configASSERT(model_now+read_delay<until);++read_count;model_now+=read_delay;return A733_THERMAL_SAMPLE;}
int a733_thermal_policy_init(struct a733_thermal_policy *p,uint64_t n,int (*notify)(void *,enum a733_thermal_level),void *cookie)
{memset(p,0,sizeof(*p));p->start_us=n;p->initialized=1;p->notify=notify;p->cookie=cookie;return 1;}
int a733_thermal_policy_cpu_limit(struct a733_thermal_policy *p,uint32_t limit)
{p->cpu_critical_millicelsius=limit;return 1;}
enum a733_thermal_level a733_thermal_policy_update(struct a733_thermal_policy *p,uint64_t n,enum a733_thermal_result result,const struct a733_thermal_sample *sample)
{(void)result;(void)sample;++p->sequence;p->seen=31;for(unsigned i=0;i<5;++i){p->seen_us[i]=n;p->millicelsius[i]=40000;}
 return critical?A733_THERMAL_CRITICAL:A733_THERMAL_NORMAL;}
uint32_t a733_thermal_policy_seen(const struct a733_thermal_policy *p){return p->seen;}
int os_cpu_clock_step(const struct a733_thermal_policy *p){(void)p;return 1;}
int gicv3_spi_rearm(const struct gicv3_spi *irq){(void)irq;return 1;}
int gicv3_spi_mask(const struct gicv3_spi *irq){(void)irq;return 1;}
void vTaskEnterCritical(void){}
void vTaskExitCritical(void){}
void board_fail_at(uint32_t site,uint32_t value,const void *caller){(void)site;(void)value;(void)caller;failed=1;}
enum a733_clock_result a733_clock_retain(struct a733_clock_leases *registry,const struct a733_clock_token *token)
{(void)registry;(void)token;return A733_CLOCK_OK;}
unsigned os_application_activity_generation(void){return 7;}
void thermal_cadence_check(unsigned mode,uint64_t now_us,uint64_t delay_us,unsigned irq)
{
    model_now=now_us;read_delay=delay_us;
    if(mode==0){
        memset(&thermal,0,sizeof(thermal));memset(&readings,0,sizeof(readings));memset(&window,0,sizeof(window));
        memset(&board,0,sizeof(board));memset(&providers,0,sizeof(providers));
        thermal_record_origin_us=0;thermal.handle=(TaskHandle_t)1;thermal.cpu_critical_millicelsius=70000;
        (void)a733_thermal_policy_cpu_limit(&thermal.policy,70000);
        board.session.deadline_us=UINT64_C(100000000);board.session.epoch=11;board.session.generation=12;
        read_count=critical=failed=0;
    }else if(mode==1||mode==4){
        critical=mode==4;atomic_store(&thermal.pending,irq!=0);(void)step();
    }else if(mode==2)os_thermal_record_begin();
    else if(mode==3)os_thermal_record_end(1);
    thermal_cadence_result[0]=thermal.next_read;thermal_cadence_result[1]=readings.next;
    thermal_cadence_result[2]=atomic_load(&readings.count);thermal_cadence_result[3]=read_count;
    thermal_cadence_result[4]=failed;thermal_cadence_result[5]=model_now;
    thermal_cadence_result[6]=thermal_record_origin_us;
}
_Noreturn void platform_panic(const char *file,unsigned line)
{(void)file;(void)line;__builtin_trap();}
