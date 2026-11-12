/* Included by checkpoint.c after the real native application definition.
 * These software owners remain live until normal ROM return and host RAM
 * restoration. No device CLOSE or platform quiescence is claimed. */
static StaticTask_t provider_client_state;
static StackType_t provider_client_stack[4096] __attribute__((aligned(16)));
static void provider_client(void *unused)
{(void)unused;platform_panic("NPU constructor task scheduled",1);}
static int provider_checkpoint_ready(void)
{
    const struct device_memory_pool *pool=app.pool;
    if(!published(pool,sizeof(*pool)) || pool->next_generation!=2 || pool->memory_class!=DEVICE_MEMORY_CLASS_NONE ||
       app.lease.slot>=DEVICE_MEMORY_MAX_ALLOCATIONS ||
       app.extent.cpu!=pool->backing.cpu || app.extent.physical!=pool->backing.physical ||
       app.extent.bytes!=A733_NPU_RESERVED_BYTES ||
       pool->clean_end!=A733_NPU_RESERVED_BYTES/DEVICE_MEMORY_PAGE)return 0;
    for(unsigned i=0;i<DEVICE_MEMORY_MAX_ALLOCATIONS;++i) {
        const struct device_memory_allocation *a=&pool->allocations[i];
        if(i==app.lease.slot) {
            if(a->owner!=app.lease.owner || a->generation!=app.lease.generation || a->first ||
               a->pages!=A733_NPU_RESERVED_BYTES/DEVICE_MEMORY_PAGE || a->state!=DEVICE_MEMORY_DEVICE)return 0;
        } else if(!zero_bytes(a,sizeof(*a)))return 0;
    }
    const struct a733_npu_startup *s=&app.startup;
    const struct npu_service *v=&app.reserved.convolution.service;
    for(unsigned i=0;i<A733_POWER_DOMAINS;++i) {
        const struct a733_power_domain *d=&app.domains.domain[i];
        if(d->state || d->count || d->delays_ready || d->command_at || d->deadline)return 0;
    }
    for(unsigned i=0;i<v->client_count;++i)
        if(v->clients[i].pending)return 0;
    return xTaskGetSchedulerState()==taskSCHEDULER_NOT_STARTED &&
        app.used==1 && !app.creating && app.client && app.pmic_task && app.bootstrap_task &&
        app.client!=app.pmic_task && app.client!=app.bootstrap_task && app.pmic_task!=app.bootstrap_task &&
        app.registry.initialized && !atomic_load(&app.registry.failed) &&
        zero_bytes(app.registry.slots,sizeof(app.registry.slots)) &&
        app.registry.next_id==1 && !atomic_load(&app.registry.busy) &&
        app.locks.initialized && !atomic_load(&app.locks.retained) &&
        app.gpu.initialized && !app.gpu.failed && !app.gpu.stage &&
        zero_bytes(app.gpu.leases,sizeof(app.gpu.leases)) &&
        app.rails.self==&app.rails && !app.rails.shared_held && !app.rails.exclusive_held &&
        zero_bytes(app.rails.slots,sizeof(app.rails.slots)) &&
        zero_bytes(app.rails.borrows,sizeof(app.rails.borrows)) &&
        app.pmic.self==&app.pmic && app.pmic.stage==0 && !app.pmic.held &&
        !app.pmic.retained && !app.pmic.failed && !app.pmic.phase &&
        !atomic_load(&app.pmic.queued) && !atomic_load(&app.pmic.busy) &&
        app.pmic.active==A733_PMIC_REQUESTS && app.pmic.next_id==1 &&
        zero_bytes(&app.pmic.clock,sizeof(app.pmic.clock)) &&
        zero_bytes(&app.pmic.pin,sizeof(app.pmic.pin)) &&
        zero_bytes(app.pmic.slots,sizeof(app.pmic.slots)) &&
        app.domains.self==&app.domains && !app.domains.running && !app.domains.failed &&
        !app.domains.supply_stage && !app.domains.supply_verified &&
        zero_bytes(&app.domains.supply_token,sizeof(app.domains.supply_token)) &&
        zero_bytes(&app.domains.supply_request,sizeof(app.domains.supply_request)) &&
        s->initialized && s->task==app.bootstrap_task && s->stage==A733_NPU_STARTUP_TOKENS &&
        !s->failed && !s->register_held && !s->entry_us && !s->ready_us &&
        !atomic_load(&s->window_open) &&
        zero_bytes(&s->clock,sizeof(s->clock)) && zero_bytes(&s->rail,sizeof(s->rail)) &&
        zero_bytes(&s->pck,sizeof(s->pck)) &&
        !atomic_load(&s->row.request) && !atomic_load(&s->row.state) && !s->row.counted &&
        app.reserved.convolution.prepared && app.reserved.convolution.bound &&
        !app.reserved.convolution.opened && !app.reserved.convolution.ready &&
        !app.reserved.convolution.job && !app.reserved.convolution.pending &&
        !atomic_load(&app.reserved.convolution.failed) &&
        v->initialized && v->worker && v->requests && v->irq_bound && !v->irq_masked &&
        !atomic_load(&v->activation_state) && !atomic_load(&v->signals->irq_pending) &&
        !atomic_load(&v->irq_completions) && !v->irq_retained && !v->has_deferred &&
        !v->has_held && !v->job_ticket && !v->running && !v->reopening &&
        !atomic_load(&app.ready) && !atomic_load(&app.worker_admitted) &&
        !atomic_load(&app.stopped);
}
static void provider_checkpoint_construct(void)
{
    TaskHandle_t client=xTaskCreateStaticAffinitySet(provider_client,"npu-check",4096,NULL,5,
        provider_client_stack,&provider_client_state,1);
    if(!client || !a733_npu_application_init(client) || !provider_checkpoint_ready())
        platform_panic("NPU constructor checkpoint",1);
    /* The static task constructor may unmask IRQs. Restore the admitted
     * preparation state before receipt publication; no EL1 timer is armed. */
    __asm__ volatile("msr daifset,#15":::"memory");
}
