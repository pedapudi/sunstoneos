#ifndef SUNSTONE_DI_PORT_H
#define SUNSTONE_DI_PORT_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
typedef uint64_t dma_addr_t;
#define ENOMEM 12
typedef uint8_t u8;typedef uint16_t u16;typedef uint32_t u32;typedef uint64_t u64;
typedef int8_t s8;typedef int16_t s16;typedef int32_t s32;typedef int64_t s64;
typedef u8 __u8;typedef u16 __u16;typedef u32 __u32;typedef u64 __u64;
typedef s8 __s8;typedef s16 __s16;typedef s32 __s32;typedef s64 __s64;
#define __KERNEL__ 1
#define CONFIG_SUNXI_DI_SINGEL_FILE 1
#define IS_ENABLED(x) (x)
#define __iomem
#define __force
#define __user
#define BIT(n) (UINT32_C(1)<<(n))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define abs(a) ((a)<0?-(a):(a))
#define clamp(a,lo,hi) (min(max((a),(lo)),(hi)))
#define likely(a) (a)
#define unlikely(a) (a)
#define ALIGN(a,n) (((a)+(n)-1)&~((n)-1))
#define DI_DEBUG(...) ((void)0)
#define DI_INFO(...) ((void)0)
#define DI_ERR(...) ((void)0)
#define TAG "di: "
#define GFP_KERNEL 0
#define _IOR(a,b,c) 0
#define _IOW(a,b,c) 0
struct list_head {struct list_head *next,*prev;};
typedef unsigned wait_queue_head_t;typedef unsigned atomic_t;
enum dma_data_direction {DMA_BIDIRECTIONAL,DMA_TO_DEVICE,DMA_FROM_DEVICE};
extern unsigned di_port_violation;
static inline u32 di_port_div(u64 *n,u32 base){if(!base){di_port_violation=1;return 0;}u32 r=(u32)(*n%base);*n/=base;return r;}
#define do_div(n,base) di_port_div(&(n),(base))
static inline u32 readl(const volatile void *p){return *(const volatile u32 *)p;}
static inline void writel(u32 v,volatile void *p){*(volatile u32 *)p=v;}
/* These Linux lifecycle paths are not part of the fixed configuration adapter.
 * If accidentally reached, generation fails rather than allocating or waiting. */
static inline void *kzalloc(size_t n,unsigned flags){(void)n;(void)flags;di_port_violation=1;return NULL;}
static inline void kfree(void *p){(void)p;di_port_violation=1;}
static inline void udelay(unsigned n){(void)n;di_port_violation=1;}
static inline unsigned di_device_get_debug_mode(void){return 0;}
#endif
