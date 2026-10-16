#include "pvr_free_list.h"
#include "reference.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct pvr_free_lists manager;
static struct pvr_gpu_events events;
static struct pvr_ccb queue;
static struct pvr_free_list_page pages[2][44];
static _Alignas(4096) unsigned char backing[100][4096];
static unsigned char table[2][44*4], record[2][64], qmem[5][128*88], responses[144][88];
static unsigned char expected_table[2][44*4], expected_record[2][64];
static unsigned allocated, calls, published_pages, recovered, response_count, stop_allocate;
static unsigned uncertain, fail_publication, fail_recovery, callback_reentry;
static uint64_t time_now;
static struct pvr_free_list_config configs[2];
static void leput(unsigned char *p,uint64_t v,unsigned n)
{for(unsigned i=0;i<n;++i)p[i]=(unsigned char)(v>>(8*i));}
static uint32_t leget(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned list(uint64_t token) {assert(token==100 || token==101);return (unsigned)(token-100);}
static int allocate_page(void *ctx,uint64_t epoch,uint64_t owner,uint64_t token,unsigned slot,struct pvr_free_list_page *out)
{
    assert(ctx==&manager && epoch==7 && owner==list(token)+1 && slot<44); ++calls;
    if(stop_allocate && allocated==stop_allocate)return 0;
    assert(allocated<100);
    *out=(struct pvr_free_list_page){backing[allocated],UINT64_C(0x10000000)+allocated*8192,allocated+1};
    ++allocated;
    if(callback_reentry)assert(pvr_free_lists_poll(&manager,time_now)==PVR_FREE_LIST_FAULT);
    return uncertain ? -1 : 1;
}
static int publish_page(void *ctx,const struct pvr_free_list_page *p)
{
    assert(ctx==&manager && p->lease && manager.lists[manager.list].acquired); ++calls;
    for(unsigned i=0;i<4096;++i)assert(!((unsigned char *)p->cpu)[i]);
    ++published_pages; return !fail_publication;
}
static int memory_read(void *ctx,uint64_t token,enum pvr_free_list_memory kind,uint32_t off,void *out,size_t n)
{assert(ctx==&manager && kind==PVR_FREE_LIST_FIRMWARE && off==28 && n==4);++calls;memcpy(out,record[list(token)]+off,n);return 1;}
static int memory_write(void *ctx,uint64_t token,enum pvr_free_list_memory kind,uint32_t off,const void *in,size_t n)
{
    assert(ctx==&manager);++calls;unsigned id=list(token);
    assert(off+n<=(kind==PVR_FREE_LIST_TABLE ? configs[id].maximum_pages*4 : 64));
    memcpy((kind==PVR_FREE_LIST_TABLE ? table[id] : record[id])+off,in,n);return 1;
}
static int memory_publish(void *ctx,uint64_t token,enum pvr_free_list_memory kind,uint32_t off,size_t n)
{assert(ctx==&manager);(void)token;(void)kind;(void)off;(void)n;++calls;return !fail_publication;}
static int recover(void *ctx,uint64_t token)
{assert(ctx==&manager);(void)token;++calls;++recovered;return fail_recovery ? -1 : 1;}
static const struct pvr_free_list_ops ops={allocate_page,publish_page,memory_read,memory_write,memory_publish,recover};
static int qr(void *ctx,enum pvr_ccb_region r,uint32_t off,void *out,size_t n)
{assert(ctx==&manager && off+n<=pvr_ccb_region_bytes(r));++calls;memcpy(out,qmem[r]+off,n);return 1;}
static int qw(void *ctx,enum pvr_ccb_region r,uint32_t off,const void *in,size_t n)
{assert(ctx==&manager && off+n<=pvr_ccb_region_bytes(r));++calls;memcpy(qmem[r]+off,in,n);return 1;}
static int qb(void *ctx,enum pvr_ccb_barrier kind)
{assert(ctx==&manager);(void)kind;++calls;return 1;}
static int qkick(void *ctx,uint32_t off,uint32_t value)
{
    assert(ctx==&manager && off==0xb00 && !value);++calls;
    unsigned next=leget(qmem[0]),slot=(next-1)&127;const unsigned char *cmd=qmem[1]+slot*88;
    assert(response_count<144);memcpy(responses[response_count++],cmd,88);
    if(leget(cmd)==(PVR_CCB_MAGIC|110u)) {
        unsigned id=leget(cmd+8)==configs[0].firmware_address ? 0 : 1;
        assert(leget(cmd+8)==configs[id].firmware_address);
        leput(record[id]+28,leget(cmd+16)-leget(cmd+20),4);leput(record[id]+48,leget(cmd+20),4);
    } else assert(leget(cmd)==(PVR_CCB_MAGIC|112u));
    leput(qmem[0]+128,next,4);leput(qmem[2]+slot*4,1,4);return 1;
}
static void setup(void)
{
    memset(&manager,0,sizeof(manager));memset(&events,0,sizeof(events));memset(&queue,0,sizeof(queue));
    memset(pages,0,sizeof(pages));memset(backing,0xa5,sizeof(backing));memset(table,0xa5,sizeof(table));
    memset(record,0xa5,sizeof(record));memset(qmem,0,sizeof(qmem));memset(responses,0,sizeof(responses));
    allocated=calls=published_pages=recovered=response_count=stop_allocate=uncertain=fail_publication=fail_recovery=callback_reentry=0;
    time_now=0;
    const struct pvr_ccb_ops qops={qr,qw,qb,qkick};assert(pvr_ccb_init(&queue,&qops,&manager,7)==PVR_CCB_OK);
    configs[0]=(struct pvr_free_list_config){{PVR_GPU_RESOURCE_FREELIST,0,1,100},0x400000000,0xc0200000,28,44,8,25,pages[0]};
    configs[1]=(struct pvr_free_list_config){{PVR_GPU_RESOURCE_FREELIST,1,2,101},0x500000000,0xc0201000,28,36,8,100,pages[1]};
    assert(pvr_free_lists_init(&manager,configs,2,25,&ops,&manager,&queue,0,100000));
}
static enum pvr_free_list_result tick(void)
{
    ++time_now;
    if(queue.phase==PVR_CCB_ACTIVE)assert(pvr_ccb_poll(&queue,time_now)==PVR_CCB_OK);
    return pvr_free_lists_poll(&manager,++time_now);
}
static void finish(void)
{
    for(unsigned i=0;i<2000;++i) {enum pvr_free_list_result r=tick();if(r==PVR_FREE_LIST_OK)return;assert(r==PVR_FREE_LIST_AGAIN);}
    assert(0);
}
static void activate(void)
{
    finish();assert(pvr_ccb_start(&queue,++time_now)==PVR_CCB_OK);
    const struct pvr_gpu_event_resource r[]={configs[0].resource,configs[1].resource};
    const struct pvr_gpu_event_handlers h={.grow=pvr_free_lists_event,.reconstruct=pvr_free_lists_event};
    assert(pvr_gpu_events_init(&events,7,r,2,&h,&manager));
}
static void request(unsigned kind,unsigned id)
{
    struct rogue_fwif_fwccb_cmd cmd={.cmd_type=(enum rogue_fwif_fwccb_cmd_type)(PVR_CCB_MAGIC|kind)};
    if(kind==103)cmd.cmd_data.cmd_free_list_gs.freelist_id=id;
    else {cmd.cmd_data.cmd_freelists_reconstruction.freelist_count=2;cmd.cmd_data.cmd_freelists_reconstruction.freelist_ids[0]=0;cmd.cmd_data.cmd_freelists_reconstruction.freelist_ids[1]=1;}
    assert(pvr_gpu_events_dispatch(&events,7,&cmd,88));
    memset(&cmd,0xff,sizeof(cmd)); /* Retained work cannot borrow callback arguments. */
}
static void reference_table(unsigned id,unsigned active)
{
    struct pvr_free_list source={.obj=expected_table[id]};
    memset(expected_table[id],0,sizeof(expected_table[id]));
    for(unsigned start=0;start<active;) {
        unsigned n=start ? configs[id].grow_pages : configs[id].initial_pages;
        uint64_t pa[44];for(unsigned i=0;i<n;++i)pa[i]=pages[id][start+i].physical;
        struct sg_table sg={pa,n};
        assert(!pvr_free_list_insert_pages_locked(&source,&sg,(configs[id].maximum_pages-start-n)*4,n));
        start+=n;
    }
    assert(!memcmp(table[id],expected_table[id],configs[id].maximum_pages*4));
}
static void grow_response(unsigned index,unsigned delta,unsigned total,unsigned ready)
{
    struct rogue_fwif_kccb_cmd cmd={.cmd_type=ROGUE_FWIF_KCCB_CMD_FREELIST_GROW_UPDATE};
    cmd.cmd_data.free_list_gs_data.freelist_fw_addr=configs[0].firmware_address;
    cmd.cmd_data.free_list_gs_data.delta_pages=delta;cmd.cmd_data.free_list_gs_data.new_pages=total;cmd.cmd_data.free_list_gs_data.ready_pages=ready;
    assert(index<response_count && !memcmp(responses[index],&cmd,88));
}
static void retained(void)
{
    assert(manager.phase==PVR_FREE_LIST_FAILED && queue.phase==PVR_CCB_FAILED);
    unsigned count=calls,leases=allocated;
    assert(pvr_free_lists_poll(&manager,++time_now)==PVR_FREE_LIST_FAULT);
    assert(calls==count && allocated==leases);
}
static void validation_cases(void)
{
    setup();memset(&manager,0,sizeof(manager));
    configs[1].gpu_address=configs[0].gpu_address+16;
    unsigned before=calls;
    assert(!pvr_free_lists_init(&manager,configs,2,25,&ops,&manager,&queue,0,100000));
    assert(!manager.phase && !manager.count && calls==before && !allocated);
    setup();assert(pvr_ccb_start(&queue,10)==PVR_CCB_OK);time_now=10;before=calls;
    assert(pvr_free_lists_poll(&manager,9)==PVR_FREE_LIST_FAULT);
    assert(manager.error==PVR_FREE_LIST_CLOCK && calls==before && !allocated);retained();
    setup();memset(&manager,0,sizeof(manager));
    configs[0].grow_pages=28;configs[0].threshold=100;
    assert(pvr_free_lists_init(&manager,configs,2,25,&ops,&manager,&queue,0,100000));
    activate();
    struct pvr_free_list ref={.max_pages=44,.initial_num_pages=28,.grow_pages=28,
        .grow_threshold=100,.fw_id=0,.free_list_gpu_addr=configs[0].gpu_address};
    memset(expected_record[0],0,64);free_list_fw_init(expected_record[0],&ref);
    assert(!memcmp(record[0],expected_record[0],64));
    assert(manager.lists[0].ready==28 && !manager.lists[0].current && leget(record[0]+16)==UINT32_MAX);
    puts("PASS GPU free-list validation: GPU table overlap refused, queue clock regression retained, all-reserve initialization agrees with original source");
}
int main(int argc,char **argv)
{
    if(argc==2 && !strcmp(argv[1],"validation")) {validation_cases();return 0;}
    assert(argc==1);

    setup();activate();assert(allocated==56 && published_pages==56);
    for(unsigned i=0;i<2;++i) {
        struct pvr_free_list ref={.max_pages=configs[i].maximum_pages,.initial_num_pages=28,.grow_pages=8,
            .grow_threshold=configs[i].threshold,.fw_id=i,.free_list_gpu_addr=configs[i].gpu_address};
        memset(expected_record[i],0,64);free_list_fw_init(expected_record[i],&ref);
        assert(!memcmp(record[i],expected_record[i],64));reference_table(i,28);
    }
    request(103,0);finish();grow_response(0,8,36,8);reference_table(0,36);
    stop_allocate=allocated+2;request(103,0);finish();grow_response(1,0,36,0);
    assert(manager.lists[0].acquired==38 && manager.lists[0].active==36);reference_table(0,36);
    unsigned held=allocated;stop_allocate=0;request(103,0);finish();grow_response(2,8,44,8);
    assert(allocated==held+6);reference_table(0,44);
    held=allocated;request(103,0);finish();grow_response(3,0,44,0);assert(allocated==held);
    memset(table[0],0xa5,44*4);memset(table[1]+(36-28)*4,0xa5,28*4);
    leput(record[0]+32,99,4);leput(record[0]+36,77,4);
    request(104,0);finish();reference_table(0,44);reference_table(1,28);assert(recovered==2);
    assert(leget(record[0]+16)==43 && !leget(record[0]+32) && !leget(record[0]+36));
    assert(leget(record[1]+16)==19);
    struct rogue_fwif_kccb_cmd reconstructed={.cmd_type=ROGUE_FWIF_KCCB_CMD_FREELISTS_RECONSTRUCTION_UPDATE};
    reconstructed.cmd_data.free_lists_reconstruction_data.freelist_count=2;
    reconstructed.cmd_data.free_lists_reconstruction_data.freelist_ids[1]=1;
    assert(response_count==5 && !memcmp(responses[4],&reconstructed,88));
    /* Firmware has consumed commands, but their uncollected return slots still
     * prevent response publication. Preserve the action until a slot is freed. */
    setup();activate();
    struct pvr_ccb_token held_tokens[PVR_CCB_HOST_SLOTS];
    struct rogue_fwif_kccb_cmd dummy={.cmd_type=ROGUE_FWIF_KCCB_CMD_FREELIST_GROW_UPDATE};
    dummy.cmd_data.free_list_gs_data.freelist_fw_addr=configs[0].firmware_address;
    dummy.cmd_data.free_list_gs_data.new_pages=28;dummy.cmd_data.free_list_gs_data.ready_pages=4;
    for(unsigned i=0;i<PVR_CCB_HOST_SLOTS;++i)
        { ++time_now; assert(pvr_ccb_submit(&queue,&dummy,time_now,time_now+100000,&held_tokens[i])==PVR_CCB_OK); }
    assert(pvr_ccb_poll(&queue,++time_now)==PVR_CCB_OK);
    request(103,0);
    while(leget(manager.response+16)!=36)assert(tick()==PVR_FREE_LIST_AGAIN);
    unsigned waiting_leases=allocated;
    for(unsigned i=0;i<2;++i)assert(tick()==PVR_FREE_LIST_AGAIN);
    assert(response_count==128 && manager.request_count==1 && allocated==waiting_leases);
    uint32_t status;assert(pvr_ccb_collect(&queue,held_tokens[0],&status)==PVR_CCB_OK && status==1);
    finish();grow_response(128,8,36,8);assert(manager.request_count==0 && allocated==waiting_leases);
    setup();uncertain=1;while(tick()==PVR_FREE_LIST_AGAIN){};
    assert(manager.error==PVR_FREE_LIST_ALLOCATION && manager.lists[0].acquired==1 && pages[0][0].lease==1 && !published_pages);retained();
    setup();callback_reentry=1;while(tick()==PVR_FREE_LIST_AGAIN){};
    assert(manager.error==PVR_FREE_LIST_REENTRY && manager.lists[0].acquired==1 && pages[0][0].lease==1);retained();
    setup();activate();request(103,0);
    while(manager.lists[0].acquired<36 || published_pages<64)assert(tick()==PVR_FREE_LIST_AGAIN);
    fail_publication=1;while(tick()==PVR_FREE_LIST_AGAIN){};
    assert(manager.error==PVR_FREE_LIST_PUBLICATION && manager.lists[0].acquired==36 && !response_count);retained();
    setup();activate();fail_recovery=1;request(104,0);while(tick()==PVR_FREE_LIST_AGAIN){};
    assert(manager.error==PVR_FREE_LIST_RECOVERY && recovered==1 && !response_count);retained();
    setup();assert(pvr_free_lists_poll(&manager,100000)==PVR_FREE_LIST_FAULT && manager.error==PVR_FREE_LIST_TIMEOUT);retained();
    puts("PASS GPU free lists: original initialization/insertion, reserve growth, retained spare pages, exact responses, reconstruction, response backpressure, allocation/publication/recovery faults and reentry");
    return 0;
}
