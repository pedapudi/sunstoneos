#ifndef SUNSTONE_A733_CPU_OFF_H
#define SUNSTONE_A733_CPU_OFF_H
#include <stdint.h>

/* Read-only entry predicates shared by the CPU owner and recovery launcher.
 * Policy/status 0x100 is FEL dynamic OFF; static OFF also holds GIC wake off. */
#define A733_CPU_DYNAMIC_OFF UINT32_C(0x100)
static inline uintptr_t a733_cpu_control_register(unsigned c)
{return UINT32_C(0x07050200)+(uintptr_t)c*4;}
static inline uintptr_t a733_cpu_mode_register(unsigned c)
{return UINT32_C(0x07050220)+(uintptr_t)c*4;}
static inline uintptr_t a733_cpu_policy_register(unsigned c)
{return UINT32_C(0x07052000)+(uintptr_t)c*0x1000;}
static inline uintptr_t a733_cpu_status_register(unsigned c)
{return a733_cpu_policy_register(c)+8;}
static inline int a733_cpu_off_status_valid(uint32_t status)
{return !(status&15);}
static inline int a733_cpu_hotplug_clear(uint32_t request)
{return !(request&1);}
static inline int a733_cpu_held_policy_valid(uint32_t policy,uint32_t control,int dynamic_allowed)
{return (dynamic_allowed && policy==A733_CPU_DYNAMIC_OFF) || (!policy && (control&2));}
static inline int a733_cpu_held_status_valid(uint32_t status,int static_off)
{return status==(static_off?0u:A733_CPU_DYNAMIC_OFF);}
#endif
