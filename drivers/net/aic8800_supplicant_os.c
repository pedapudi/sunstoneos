#include "aic8800_supplicant_port.h"
#include "aic8800_supplicant.h"
#include "utils/common.h"
#include "utils/eloop.h"
static struct aic8800_supplicant *active;
int aic8800_supplicant_os_enter(struct aic8800_supplicant *s)
{if(!s||active||s->entered||s->state==AIC8800_SUPPLICANT_FAULT)return 0;active=s;s->entered=1;return 1;}
void aic8800_supplicant_os_leave(struct aic8800_supplicant *s)
{if(active==s){s->entered=0;active=0;}}
void *os_memcpy(void *d,const void *s,size_t n)
{uint8_t *a=d;const uint8_t *b=s;for(size_t i=0;i<n;++i)a[i]=b[i];return d;}
void *os_memmove(void *d,const void *s,size_t n)
{uint8_t *a=d;const uint8_t *b=s;if((uintptr_t)a>(uintptr_t)b)while(n){--n;a[n]=b[n];}else for(size_t i=0;i<n;++i)a[i]=b[i];return d;}
void *os_memset(void *d,int v,size_t n)
{uint8_t *a=d;for(size_t i=0;i<n;++i)a[i]=(uint8_t)v;return d;}
int os_memcmp(const void *a,const void *b,size_t n)
{const uint8_t *x=a,*y=b;for(size_t i=0;i<n;++i)if(x[i]!=y[i])return x[i]<y[i]?-1:1;return 0;}
size_t os_strlen(const char *s){size_t n=0;while(s[n])++n;return n;}
int os_strcmp(const char *a,const char *b){while(*a&&*a==*b){++a;++b;}return (uint8_t)*a-(uint8_t)*b;}
int os_strncmp(const char *a,const char *b,size_t n){while(n&&*a&&*a==*b){++a;++b;--n;}return n?(uint8_t)*a-(uint8_t)*b:0;}
char *os_strchr(const char *s,int c){do{if(*s==c)return (char *)s;}while(*s++);return 0;}
char *os_strrchr(const char *s,int c){const char *last=0;do{if(*s==c)last=s;}while(*s++);return (char *)last;}
char *os_strstr(const char *s,const char *needle){size_t n=os_strlen(needle);for(;*s;++s)if(!os_strncmp(s,needle,n))return (char *)s;return n?0:(char *)s;}
static uint8_t *block(unsigned i)
{return i<AIC8800_SUPPLICANT_SMALL_SLOTS?active->small[i]:active->large[i-AIC8800_SUPPLICANT_SMALL_SLOTS];}
static size_t capacity(unsigned i){return i<AIC8800_SUPPLICANT_SMALL_SLOTS?2048:16384;}
void *os_malloc(size_t n)
{
    if(!active||!n||n>16384)return 0;
    for(unsigned i=n>2048?AIC8800_SUPPLICANT_SMALL_SLOTS:0;i<AIC8800_SUPPLICANT_SMALL_SLOTS+AIC8800_SUPPLICANT_LARGE_SLOTS;++i)
        if(!active->used[i]){active->used[i]=(uint16_t)n;return block(i);}
    aic8800_supplicant_stop(active);return 0;
}
void *os_zalloc(size_t n){void *p=os_malloc(n);if(p)os_memset(p,0,n);return p;}
void os_free(void *p)
{
    if(!p||!active)return;
    for(unsigned i=0;i<AIC8800_SUPPLICANT_SMALL_SLOTS+AIC8800_SUPPLICANT_LARGE_SLOTS;++i)
        if(p==block(i)&&active->used[i]){aic8800_security_zero(p,capacity(i));active->used[i]=0;return;}
    aic8800_supplicant_stop(active);
}
void *os_realloc(void *p,size_t n)
{
    if(!p)return os_malloc(n);
    if(!n){os_free(p);return 0;}
    if(!active)return 0;
    for(unsigned i=0;i<AIC8800_SUPPLICANT_SMALL_SLOTS+AIC8800_SUPPLICANT_LARGE_SLOTS;++i)
        if(p==block(i)&&active->used[i]){
            if(n<=capacity(i)){active->used[i]=(uint16_t)n;return p;}
            void *next=os_malloc(n);if(next){os_memcpy(next,p,active->used[i]);os_free(p);}return next;
        }
    aic8800_supplicant_stop(active);return 0;
}
char *os_strdup(const char *s){size_t n=os_strlen(s)+1;char *p=os_malloc(n);if(p)os_memcpy(p,s,n);return p;}
int os_get_random(unsigned char *p,size_t n)
{
    if(!active||!active->config.entropy||!n||n>256)return -1;
    if(active->config.entropy(active->config.entropy_context,p,n)!=1){aic8800_security_zero(p,n);aic8800_supplicant_stop(active);return -1;}return 0;
}
int os_get_reltime(struct os_reltime *t)
{
    if(!active||!active->config.ticks_per_second)return -1;
    uint64_t hz=active->config.ticks_per_second,seconds=active->last_now/hz;
    if(seconds>LONG_MAX)return -1;
    t->sec=(long)seconds;t->usec=(long)((active->last_now%hz)*1000000/hz);return 0;
}
int os_get_time(struct os_time *t){struct os_reltime r;if(os_get_reltime(&r))return -1;t->sec=r.sec;t->usec=r.usec;return 0;}
int eloop_register_timeout(unsigned int sec,unsigned int usec,eloop_timeout_handler handler,void *ctx,void *arg)
{
    if(!active||!handler||usec>=1000000)return -1;
    uint64_t hz=active->config.ticks_per_second;
    if(sec>UINT64_MAX/hz)return -1;
    uint64_t ticks=(uint64_t)sec*hz,partial=((uint64_t)usec*hz+999999)/1000000;
    if(partial>UINT64_MAX-ticks||ticks+partial>UINT64_MAX-active->last_now)return -1;
    for(unsigned i=0;i<AIC8800_SUPPLICANT_TIMERS;++i)if(!active->timers[i].handler){
        active->timers[i]=(struct aic8800_supplicant_timer){handler,ctx,arg,active->last_now+ticks+partial};return 0;}
    aic8800_supplicant_stop(active);return -1;
}
int eloop_cancel_timeout(eloop_timeout_handler handler,void *ctx,void *arg)
{
    if(!active)return 0;
    int count=0;
    for(unsigned i=0;i<AIC8800_SUPPLICANT_TIMERS;++i){struct aic8800_supplicant_timer *t=&active->timers[i];
        if(t->handler==handler&&(ctx==ELOOP_ALL_CTX||t->context==ctx)&&(arg==ELOOP_ALL_CTX||t->argument==arg)){*t=(struct aic8800_supplicant_timer){0};++count;}}
    return count;
}
int eloop_is_timeout_registered(eloop_timeout_handler handler,void *ctx,void *arg)
{
    if(!active)return 0;
    for(unsigned i=0;i<AIC8800_SUPPLICANT_TIMERS;++i){struct aic8800_supplicant_timer *t=&active->timers[i];if(t->handler==handler&&t->context==ctx&&t->argument==arg)return 1;}return 0;
}
int aic8800_supplicant_os_timer(struct aic8800_supplicant *s)
{
    if(active!=s)return 0;
    for(unsigned i=0;i<AIC8800_SUPPLICANT_TIMERS;++i)if(s->timers[i].handler&&s->timers[i].due<=s->last_now){
        struct aic8800_supplicant_timer t=s->timers[i];s->timers[i]=(struct aic8800_supplicant_timer){0};t.handler(t.context,t.argument);return 1;}
    return 0;
}

_Noreturn void aic8800_supplicant_abort(void)
{
    if(active)aic8800_supplicant_stop(active);
    /* An upstream invariant failure cannot continue with an unbounded write.
     * Native exception handling must retain this failed owner for recovery. */
    __builtin_trap();
}
void *os_memdup(const void *p,size_t n)
{void *copy=os_malloc(n);if(copy)os_memcpy(copy,p,n);return copy;}
int os_memcmp_const(const void *a,const void *b,size_t n)
{
    const uint8_t *x=a,*y=b;volatile unsigned difference=0;
    for(size_t i=0;i<n;++i)difference|=x[i]^y[i];
    return difference!=0;
}
int os_snprintf(char *p,size_t n,const char *format,...)
{
    /* Reachable callers format diagnostic titles only. Diagnostics are disabled;
     * report formatting unavailable, without creating a secret-bearing log. */
    (void)format;if(n&&p)*p=0;return -1;
}
