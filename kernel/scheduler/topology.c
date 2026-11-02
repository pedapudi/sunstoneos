/* SPDX-License-Identifier: GPL-2.0-only */
#include "sunstone_cpu_topology.h"
static uint64_t cpu_mask(size_t n)
{ return n == 64 ? UINT64_MAX : (UINT64_C(1) << n) - 1; }
int ss_cpu_topology_validate(const struct ss_cpu_topology *t)
{
    if (!t || t->version != SS_TOPOLOGY_VERSION || !t->cpus || !t->policy || !t->cpu_count ||
        t->cpu_count > SS_CPU_MAX || !t->domains || !t->domain_count ||
        t->domain_count > SS_DOMAIN_MAX || t->cache_count > SS_CACHE_MAX ||
        (t->cache_count && !t->caches)) return SS_TOPOLOGY_INVALID;
    uint64_t all=cpu_mask(t->cpu_count), covered=0;
    const struct ss_cpu_policy *p=t->policy;
    if (!p->admitted || (p->admitted & ~all) ||
        ((p->reserved|p->service|p->application|p->irq) & ~p->admitted) ||
        ((p->service|p->application|p->irq) & p->reserved)) return SS_TOPOLOGY_INVALID;
    for (size_t i=0;i<t->domain_count;i++) {
        uint64_t m=t->domains[i].cpus;
        if (!m || (m & ~all) || (m & covered)) return SS_TOPOLOGY_INVALID;
        const struct ss_cpu_domain *d=&t->domains[i];
        if(d->energy_count>SS_ENERGY_POINT_MAX ||
            (!!d->energy != !!d->energy_count)) return SS_TOPOLOGY_INVALID;
        if(d->energy_count && (!t->energy_source || !t->energy_source[0] ||
            !t->opp_frequency_hz || d->opp_table_id==SS_ID_NONE)) return SS_TOPOLOGY_INVALID;
        for(size_t k=0;k<d->energy_count;k++) {
            const struct ss_cpu_energy_point *e=&d->energy[k];
            if(!e->frequency_hz || !e->power_uw ||
                t->opp_frequency_hz(d->opp_table_id,e->opp_id)!=e->frequency_hz)
                return SS_TOPOLOGY_INVALID;
            for(size_t j=0;j<k;j++)
                if(e->opp_id==d->energy[j].opp_id || e->frequency_hz==d->energy[j].frequency_hz)
                    return SS_TOPOLOGY_INVALID;
        }
        covered |= m;
    }
    if (covered != all) return SS_TOPOLOGY_INVALID;
    for (size_t i=0;i<t->cache_count;i++) {
        const struct ss_cpu_cache *c=&t->caches[i];
        if (!c->cpus || (c->cpus & ~all) || !c->level || c->level>3 ||
            c->type<SS_CACHE_INSTRUCTION || c->type>SS_CACHE_UNIFIED ||
            (c->line_bytes && (c->line_bytes & (c->line_bytes-1))) ||
            (c->bytes && (!c->line_bytes || c->bytes % c->line_bytes)))
            return SS_TOPOLOGY_INVALID;
    }
    for (size_t i=0;i<t->cpu_count;i++) {
        const struct ss_cpu_info *c=&t->cpus[i]; uint64_t bit=UINT64_C(1)<<i;
        if (c->logical_id!=i || (c->affinity & ~SS_MPIDR_AFFINITY_MASK) ||
            c->performance_domain>=t->domain_count ||
            !(t->domains[c->performance_domain].cpus & bit) ||
            !c->siblings || !(c->siblings & bit) || (c->siblings & ~all) ||
            (c->flags & ~(SS_CPU_CAPACITY_MAX_VALID|SS_CPU_CAPACITY_EFFECTIVE_VALID|SS_CPU_ENERGY_VALID)) ||
            c->capacity_max>1024 || c->capacity_effective>1024 ||
            (!!c->capacity_max != !!(c->flags & SS_CPU_CAPACITY_MAX_VALID)) ||
            (!!c->capacity_effective != !!(c->flags & SS_CPU_CAPACITY_EFFECTIVE_VALID)) ||
            (c->capacity_effective && (!c->capacity_max || c->capacity_effective>c->capacity_max)) ||
            ((c->flags & 3u) && (!t->calibration_epoch || !t->calibration_source || !t->capacity_workload)) ||
            ((c->flags & SS_CPU_CAPACITY_EFFECTIVE_VALID) && !t->operating_epoch) ||
            (!!(c->flags & SS_CPU_ENERGY_VALID) != !!t->domains[c->performance_domain].energy_count)) return SS_TOPOLOGY_INVALID;
        const uint16_t ids[]={c->l1i,c->l1d,c->l2,c->l3};
        for (unsigned k=0;k<4;k++) if(ids[k]!=SS_ID_NONE) {
            if(ids[k]>=t->cache_count) return SS_TOPOLOGY_INVALID;
            const struct ss_cpu_cache *cache=&t->caches[ids[k]];
            unsigned level=k<2?1:k;
            if (!(cache->cpus & bit) || cache->level!=level ||
                (k==0 && cache->type!=SS_CACHE_INSTRUCTION && cache->type!=SS_CACHE_UNIFIED) ||
                (k==1 && cache->type!=SS_CACHE_DATA && cache->type!=SS_CACHE_UNIFIED) ||
                (k>=2 && cache->type!=SS_CACHE_UNIFIED)) return SS_TOPOLOGY_INVALID;
        }
        for(size_t j=0;j<t->cpu_count;j++) {
            const struct ss_cpu_info *other=&t->cpus[j];
            if(i!=j && c->affinity==other->affinity) return SS_TOPOLOGY_INVALID;
            if (!!(c->siblings & (UINT64_C(1)<<j)) != (c->physical_core==other->physical_core) ||
                ((c->siblings & (UINT64_C(1)<<j)) &&
                 (c->siblings!=other->siblings || c->cluster!=other->cluster ||
                  c->performance_domain!=other->performance_domain || c->midr!=other->midr ||
                  c->l1i!=other->l1i || c->l1d!=other->l1d ||
                  c->l2!=other->l2 || c->l3!=other->l3))) return SS_TOPOLOGY_INVALID;
        }
    }
    /* Every declared sharing member must link back to the same cache. */
    for(size_t k=0;k<t->cache_count;k++) {
        uint64_t members=0;
        for(size_t i=0;i<t->cpu_count;i++) {
            const struct ss_cpu_info *c=&t->cpus[i];
            if(c->l1i==k || c->l1d==k || c->l2==k || c->l3==k)
                members|=UINT64_C(1)<<i;
        }
        if(members!=t->caches[k].cpus) return SS_TOPOLOGY_INVALID;
    }
    return SS_TOPOLOGY_OK;
}
int ss_cpu_from_mpidr(const struct ss_cpu_topology *t,uint64_t mpidr,unsigned *cpu)
{
    if(!t || !cpu || t->cpu_count>SS_CPU_MAX) return SS_TOPOLOGY_INVALID;
    for(size_t i=0;i<t->cpu_count;i++) if(t->cpus[i].affinity==(mpidr & SS_MPIDR_AFFINITY_MASK)) {
        *cpu=(unsigned)i; return SS_TOPOLOGY_OK;
    }
    return SS_TOPOLOGY_INVALID;
}
int ss_cpu_topology_query(const struct ss_cpu_topology *t,struct ss_cpu_info *out,size_t cap,size_t *count)
{
    if(!t || !count || t->cpu_count>SS_CPU_MAX) return SS_TOPOLOGY_INVALID;
    *count=t->cpu_count;
    if(cap<t->cpu_count) return SS_TOPOLOGY_SPACE;
    if(!out) return SS_TOPOLOGY_INVALID;
    for(size_t i=0;i<t->cpu_count;i++) out[i]=t->cpus[i];
    return SS_TOPOLOGY_OK;
}
int ss_cpu_work_split(const struct ss_cpu_topology *t,uint64_t units,uint64_t mask,
    uint32_t quantum,struct ss_work_range *out,size_t cap,size_t *count)
{
    if(!t || !count || !t->cpu_count || t->cpu_count>SS_CPU_MAX || !quantum ||
        !mask || (mask & ~(t->policy->application & ~t->policy->reserved))) return SS_TOPOLOGY_INVALID;
    unsigned ids[SS_CPU_MAX], order[SS_CPU_MAX], scratch[SS_CPU_MAX], n=0;
    uint64_t weights=0, shares[SS_CPU_MAX], remainder[SS_CPU_MAX];
    for(size_t i=0;i<t->cpu_count;i++) if(mask & (UINT64_C(1)<<i)) ids[n++]=(unsigned)i;
    *count=n;
    if(cap<n) return SS_TOPOLOGY_SPACE;
    if(!out) return SS_TOPOLOGY_INVALID;
    for(unsigned i=0;i<n;i++) {
        const struct ss_cpu_info *c=&t->cpus[ids[i]];
        if(!(c->flags & SS_CPU_CAPACITY_EFFECTIVE_VALID) || !c->capacity_effective)
            return SS_TOPOLOGY_UNCALIBRATED;
        if(c->capacity_effective>1024) return SS_TOPOLOGY_INVALID;
        /* Sibling throughput requires shared-resource calibration, which API
         * version one cannot describe. A single sibling remains usable. */
        if(c->siblings & mask & ~(UINT64_C(1)<<ids[i])) return SS_TOPOLOGY_UNCALIBRATED;
        weights+=c->capacity_effective;
    }
    uint64_t quanta=units/quantum, assigned=0;
    for(unsigned i=0;i<n;i++) {
        uint64_t w=t->cpus[ids[i]].capacity_effective;
        /* weights <= 65536 and w <= 1024 bound this product. Splitting the
         * quotient avoids multiplying a potentially UINT64_MAX job by w. */
        uint64_t product=(quanta%weights)*w;
        shares[i]=(quanta/weights)*w+product/weights;
        remainder[i]=product%weights; assigned+=shares[i]; order[i]=i;
    }
    /* Stable radix order of descending remainders preserves ascending CPU ID
     * ties. Two byte passes suffice because remainder < weights <= 65536. */
    for(unsigned shift=0;shift<16;shift+=8) {
        unsigned buckets[256]={0}, offset=0;
        for(unsigned i=0;i<n;i++) buckets[255-((remainder[order[i]]>>shift)&255)]++;
        for(unsigned b=0;b<256;b++) { unsigned size=buckets[b]; buckets[b]=offset; offset+=size; }
        for(unsigned i=0;i<n;i++) scratch[buckets[255-((remainder[order[i]]>>shift)&255)]++]=order[i];
        for(unsigned i=0;i<n;i++) order[i]=scratch[i];
    }
    uint64_t left=quanta-assigned;
    if(left>=n) return SS_TOPOLOGY_OVERFLOW;
    for(unsigned i=0;i<left;i++) shares[order[i]]++;
    uint64_t begin=0;
    for(unsigned i=0;i<n;i++) {
        uint64_t length=shares[i]*quantum;
        if(i==n-1) length+=units%quantum;
        out[i]=(struct ss_work_range){begin,begin+length,ids[i],0}; begin+=length;
    }
    return SS_TOPOLOGY_OK;
}
