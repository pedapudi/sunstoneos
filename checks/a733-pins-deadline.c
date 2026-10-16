/* Reuse the register/lock fixture; execute only late-completion cases. */
#define main pins_original_cases
#include "a733-pins.c"
#undef main

static unsigned inject_on_read, inject_function, regress, gpio_read, read_min_writes;
static int final_write(void *cookie, uintptr_t address, uint32_t value)
{
    int result=write_reg(cookie,address,value);
    if (!inject_on_read && address==A733_PINS_BASE+0x100 &&
        ((value>>20)&15)==inject_function) now=regress ? 0 : 100;
    return result;
}
static int final_read(void *cookie, uintptr_t address, uint32_t *value)
{
    int result=read_reg(cookie,address,value);
    if (result && ((gpio_read && address==A733_PINS_BASE+0x110) ||
        (inject_on_read && address==A733_PINS_BASE+0x100 && writes && writes>=read_min_writes &&
         ((*value>>20)&15)==inject_function))) now=regress ? 0 : 100;
    return result;
}
static void inject(unsigned readback,unsigned function,unsigned backwards,unsigned input)
{
    inject_on_read=readback;inject_function=function;regress=backwards;gpio_read=input;read_min_writes=0;
    manager.operations.write=final_write;manager.operations.read=final_read;
}
int main(void)
{
    for (unsigned readback=0;readback<2;++readback) {
        setup(); now=50; expected_writes(1u<<5);
        struct a733_pin_request r=request(5,3); struct a733_pin_lease lease={0};
        inject(readback,3,0,0);
        assert(a733_pins_acquire(&manager,1,&r,1,&lease,100)==A733_PINS_RECOVERY);
        assert(((words[0x100/4]>>20)&15)==3 && lease.id);check_failed(&lease);

        setup(); now=50; expected_writes(1u<<5); lease=(struct a733_pin_lease){0};
        configure_field(5,0,1,2);r.expected.function=0;
        assert(a733_pins_acquire(&manager,1,&r,1,&lease,100)==A733_PINS_OK);
        inject(readback,0,0,0);
        assert(a733_pins_release(&manager,&lease,1,100)==A733_PINS_RECOVERY);
        assert(((words[0x100/4]>>20)&15)==0 && lease.id);check_failed(&lease);
    }
    /* The final restore can be an unchanged mux read after earlier writes. */
    setup();now=50;expected_writes(1u<<5);
    struct a733_pin_request r=request(5,3);struct a733_pin_lease lease={0};
    assert(a733_pins_acquire(&manager,1,&r,1,&lease,100)==A733_PINS_OK);
    /* Expire on the final unchanged mux read after three restoration writes. */
    inject(1,15,0,0);read_min_writes=writes+3;
    assert(a733_pins_release(&manager,&lease,1,100)==A733_PINS_RECOVERY);check_failed(&lease);

    setup();now=50;expected_writes(1u<<5);r=request(5,0);lease=(struct a733_pin_lease){0};
    assert(a733_pins_acquire(&manager,1,&r,1,&lease,100)==A733_PINS_OK);
    uint32_t levels=0xabcdef01;inject(0,17,0,1);
    assert(a733_pins_read_inputs(&manager,&lease,1u<<5,&levels,100)==A733_PINS_BUSY);
    assert(levels==0xabcdef01 && !manager.failed);check_held(&lease,1);

    setup();now=50;expected_writes(1u<<5);r=request(5,3);lease=(struct a733_pin_lease){0};
    inject(0,3,1,0);
    assert(a733_pins_acquire(&manager,1,&r,1,&lease,100)==A733_PINS_RECOVERY && lease.id);
    now=50;check_failed(&lease);
    puts("Pin deadlines: late final acquire/release, unchanged restore, GPIO output preservation, and clock regression passed");
}
