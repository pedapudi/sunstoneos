#include "a733_cpu_off.h"
#include "refusal.h"

extern _Noreturn void card_fel_refuse(unsigned,uintptr_t,uint64_t);
static uint32_t read32(uintptr_t address)
{return *(volatile const uint32_t *)address;}

void card_fel_check_secondaries(void)
{
    for(unsigned c=1;c<8;++c) {
        uintptr_t address=a733_cpu_status_register(c);
        uint32_t sample=read32(address);
        if(!a733_cpu_off_status_valid(sample))card_fel_refuse(CARD_FEL_REFUSE_OFF_STATUS+c,address,sample);
        address=a733_cpu_control_register(c);
        uint32_t control=read32(address);
        if(!a733_cpu_hotplug_clear(control))card_fel_refuse(CARD_FEL_REFUSE_CONTROL+c,address,control);
        address=a733_cpu_mode_register(c);
        sample=read32(address);
        if(!a733_cpu_hotplug_clear(sample))card_fel_refuse(CARD_FEL_REFUSE_MODE+c,address,sample);
        address=a733_cpu_policy_register(c);
        uint32_t policy=read32(address);
        if(!a733_cpu_held_policy_valid(policy,control,1)) {
            if(!policy)card_fel_refuse(CARD_FEL_REFUSE_STATIC_WAKE_HOLD+c,a733_cpu_control_register(c),control);
            card_fel_refuse(CARD_FEL_REFUSE_POLICY+c,address,policy);
        }
        address=a733_cpu_status_register(c);
        sample=read32(address);
        if(!a733_cpu_held_status_valid(sample,policy!=A733_CPU_DYNAMIC_OFF))
            card_fel_refuse(CARD_FEL_REFUSE_POLICY_STATUS+c,address,sample);
    }
}
