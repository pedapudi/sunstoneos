/* Execute the filesystem against an independent in-memory FAT32 card and
 * suspend the application at each owner-event and sector-write boundary. */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "storage-owner-model.h"

static uint8_t *disk, *pristine, *marked;
static size_t disk_bytes;
static uint32_t fat1, fat2, fat_sectors, data_start, clusters, root_cluster, spc, fats;
static uint8_t cluster_buffer[65536], window[2*FWR_FAT_WINDOW_SECTORS*512], file_buffer[8192];
static struct fwr_volume volume;
static struct fwr_file file;
static jmp_buf stopped;
static int inject, point_count, oracle_dirty, tracking, writes, events, reference_mode, fail_at, write_attempts;
static uint32_t get16(const uint8_t *p){return p[0]|(uint32_t)p[1]<<8;}
static uint32_t get32(const uint8_t *p){return get16(p)|get16(p+2)<<16;}
static uint32_t fat(uint32_t c){return get32(disk+(size_t)fat1*512+c*4)&0xfffffff;}
static void point(void){if(tracking&&++point_count==inject)longjmp(stopped,1);}
static int read_card(void *c,uint64_t s,uint32_t n,uint8_t *p)
{(void)c;assert(s+n<=disk_bytes/512);memcpy(p,disk+s*512,(size_t)n*512);return 1;}
static int raw_write(void *c,uint64_t s,uint32_t n,const uint8_t *p)
{(void)c;assert(reference_mode);assert(s+n<=disk_bytes/512);memcpy(disk+s*512,p,(size_t)n*512);if(tracking)++writes;return 1;}
static int event(void *c,unsigned e,unsigned op,uint32_t *token)
{
    (void)c;point();
    int ok=retirement_event(e,op,token);
    assert(ok||fail_at);
    if(e==FWR_TX_COMMIT&&ok)oracle_dirty=0;
    if(tracking)++events;
    point();return ok;
}
static int write_card(void *c,uint64_t s,uint32_t n,const uint8_t *p,unsigned phase,uint32_t token)
{
    (void)c;assert(s+n<=disk_bytes/512);point();
    /* FAT, FSInfo and existing root entries are independently identifiable.
     * A new directory cluster is harmless until its FAT link is published. */
    int metadata=(s>=fat1&&s<data_start)||s==1||
        (s>=data_start+(root_cluster-2)*spc&&s<data_start+(root_cluster-1)*spc);
    if(s>=data_start+(root_cluster-2)*spc&&s<data_start+(root_cluster-1)*spc)
        assert(phase==FWR_PHASE_DIRECTORY);
    if(metadata)oracle_dirty=1;
    assert(retirement_write(phase,token));
    if(metadata)assert((atomic_load(&retirement)&255u)!=FWR_PHASE_DATA);
    point();
    if(tracking&&++write_attempts==fail_at){oracle_dirty=1;retirement_written(0);return 0;}
    memcpy(disk+s*512,p,(size_t)n*512);
    retirement_written(1);
    if(tracking)++writes;
    point();return 1;
}
static void mark_chain(uint32_t first,uint32_t size,int directory)
{
    uint32_t c=first,n=0,offset=0;
    if(!first){assert(!size&&!directory);return;}
    do {
        assert(c>=2&&c<clusters+2&&!marked[c]);marked[c]=1;++n;
        if(directory){
            uint8_t *p=disk+(size_t)(data_start+(c-2)*spc)*512;
            for(unsigned off=0;off<spc*512;off+=32){
                uint8_t *e=p+off;
                if(!e[0])break;
                if(e[0]==0xe5||e[0]=='.'||(e[11]&0x0f)==0x0f||(e[11]&8))continue;
                mark_chain(get16(e+20)<<16|get16(e+26),get32(e+28),(e[11]&16)!=0);
            }
        }
        if(!directory){
            uint8_t *p=disk+(size_t)(data_start+(c-2)*spc)*512;
            for(unsigned i=0;i<spc*512&&offset<size;++i,++offset){
                /* Every visible file is either the previously committed 0x51
                 * prefix, its 0x93 append/replacement, or the new 0x72 file. */
                if(size==700)assert(p[i]==0x51);
                else if(size==1536)assert(p[i]==0x93);
                else if(size==4536)assert(p[i]==(offset<3000?0x51:0x93));
                else {assert(size==3000);assert(p[i]==0x51||p[i]==0x72);}
            }
        }
        c=fat(c);
    } while(c<0xffffff8);
    if(!directory)assert(n==((uint64_t)size+spc*512-1)/(spc*512));
}
static void consistent_disk(void)
{
    if(fats==2)assert(!memcmp(disk+(size_t)fat1*512,disk+(size_t)fat2*512,(size_t)fat_sectors*512));
    memset(marked,0,clusters+2);mark_chain(root_cluster,0,1);
    for(uint32_t c=2;c<clusters+2;++c)assert((fat(c)!=0)==(marked[c]!=0));
}
static void reset_owner(void)
{
    memset(&transaction,0,sizeof transaction);atomic_store(&retirement,0);
    atomic_store(&storage_state,SD_OPEN);atomic_store(&closed,0);atomic_store(&posting,0);
    loader_slot=writer_slot=NULL;close_calls=0;close_ok=1;
    atomic_store(&storage_quiesce.result,OS_QUIESCE_PENDING);
    atomic_store(&storage_quiesce.requested,0);atomic_store(&storage_quiesce.fault_state,0);
    storage_quiesce.site=storage_quiesce.value=storage_quiesce.visited=0;requested=failing=0;
    writes_enabled=1;storage_quiesce.registered=1;assert(retirement_enable());
}
static void mounted(void)
{
    struct fwr_config c={.device={.read=read_card,.write=raw_write,.sectors=disk_bytes/512,
        .au_sectors=8,.max_write_sectors=64,.transaction=event,.write_classified=write_card},
        .base=0,.sectors=disk_bytes/512,.cluster_buffer=cluster_buffer,.cluster_buffer_bytes=sizeof cluster_buffer,.fat_window=window};
    if(reference_mode)c.device.transaction=NULL,c.device.write_classified=NULL;
    assert(!fwr_mount(&volume,&c));
}
static void prepare(int operation)
{
    tracking=0;oracle_dirty=0;reset_owner();memcpy(disk,pristine,disk_bytes);mounted();
    if(operation!=0&&operation!=7&&operation!=8){
        assert(!fwr_open(&volume,&file,"/OLD.BIN",FWR_O_CREATE,0,file_buffer,sizeof file_buffer));
        memset(file_buffer,0x51,3000);file.size=3000;
        assert(!fwr_close(&file));
    }
    if(operation==0||operation==7||operation==8){
        assert(!fwr_open(&volume,&file,"/NEW.BIN",FWR_O_CREATE,0,file_buffer,sizeof file_buffer));
        uint8_t bytes[6000];size_t n=operation==7?sizeof bytes:3000;
        memset(bytes,0x72,n);assert(fwr_write(&file,bytes,n)==n);
    } else if(operation==1||operation==2||operation==3){
        assert(!fwr_open(&volume,&file,"/OLD.BIN",operation==2?FWR_O_REPLACE:FWR_O_APPEND,0,file_buffer,sizeof file_buffer));
        if(operation!=3){uint8_t bytes[1536];memset(bytes,0x93,sizeof bytes);assert(fwr_write(&file,bytes,sizeof bytes)==sizeof bytes);}
    }
    point_count=writes=events=write_attempts=0;oracle_dirty=0;tracking=1;
}
static void operation_run(int operation)
{
    int r=0;
    if(operation<=2)r=fwr_sync(&file);
    else if(operation==3)r=fwr_truncate(&file,700);
    else if(operation==4)r=fwr_unlink(&volume,"/OLD.BIN");
    else if(operation==5)r=fwr_mkdir(&volume,"/DIR");
    else if(operation==6)r=fwr_rename(&volume,"/OLD.BIN","/RENAMED.BIN");
    else if(operation==7){assert(fwr_service(&file,1)>=0);}
    else if(operation==8){}              /* bytes only in the suspended task's RAM */
    else r=fwr_checkpoint(&volume);
    if(fail_at)assert(r<0);else assert(!r);point();
}
static void return_without_application(void)
{
    tracking=0;requested=failing=1;atomic_store(&storage_quiesce.requested,1);
    /* The owner has no usable filesystem object or application lock. */
    memset(&volume,0xa5,sizeof volume);memset(&file,0x5a,sizeof file);
    enum os_quiesce_result q=os_quiesce_step(NULL);
    if(oracle_dirty){
        assert(q==OS_QUIESCE_FAULT&&close_calls==0);
        assert(storage_quiesce.value==1&&storage_quiesce.site>=0x1921&&storage_quiesce.site<=0x192c);
    } else {
        assert(q==OS_QUIESCE_PENDING);assert(owner_quiesce_step());
        assert(storage_quiesce_step(&storage_quiesce)==OS_QUIESCE_DONE&&close_calls==1);
        consistent_disk();
    }
}
static void protocol_cases(void)
{
    reset_owner();uint32_t token=0;assert(retirement_event(FWR_TX_BEGIN,FWR_OP_MOUNT,&token));
    assert(retirement_event(FWR_TX_COMMIT,FWR_OP_MOUNT,&token));
    assert(retirement_event(FWR_TX_BEGIN,FWR_OP_SYNC,&token));
    assert(retirement_write(FWR_PHASE_FAT1,token));retirement_written(0);
    assert(!retirement_event(FWR_TX_COMMIT,FWR_OP_SYNC,&token));
    assert((atomic_load(&retirement)&255)==FWR_PHASE_FAILED);
    reset_owner();assert(retirement_event(FWR_TX_BEGIN,FWR_OP_MOUNT,&token));
    assert(retirement_event(FWR_TX_COMMIT,FWR_OP_MOUNT,&token));
    assert(retirement_event(FWR_TX_BEGIN,FWR_OP_SYNC,&token));
    assert(retirement_write(FWR_PHASE_FAT1,token));uint32_t stale=token+1;
    assert(!retirement_event(FWR_TX_COMMIT,FWR_OP_SYNC,&stale));
    assert((atomic_load(&retirement)&255)==FWR_PHASE_FAT1);
    assert(retirement_event(FWR_TX_ABORT,FWR_OP_SYNC,&token)==0);
    assert((atomic_load(&retirement)&255)==FWR_PHASE_FAILED);
    reset_owner();assert(retirement_write(FWR_PHASE_RAW,0));retirement_written(1);
    assert((atomic_load(&retirement)&255)==FWR_PHASE_RAW);
    transaction.session=UINT32_MAX;assert(!retirement_enable());
    reset_owner();transaction.serial=UINT32_MAX;assert(!retirement_event(FWR_TX_BEGIN,FWR_OP_MOUNT,&token));
}
int main(int argc,char **argv)
{
    assert(argc==2||argc==3);reference_mode=argc==3;FILE *f=fopen(argv[1],"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));disk_bytes=(size_t)ftell(f);rewind(f);
    disk=malloc(disk_bytes);pristine=malloc(disk_bytes);assert(disk&&pristine);
    assert(fread(pristine,1,disk_bytes,f)==disk_bytes);fclose(f);
    spc=pristine[13];fats=pristine[16];fat1=get16(pristine+14);fat_sectors=get32(pristine+36);fat2=fat1+fat_sectors;
    data_start=fat1+fats*fat_sectors;root_cluster=get32(pristine+44);clusters=(get32(pristine+32)-data_start)/spc;
    marked=malloc(clusters+2);assert(marked);unsigned cases=0;assert(os_sd_quiesce_register());
    for(int op=0;op<10;++op){
        inject=0;prepare(op);operation_run(op);int points=point_count;
        assert(events==(reference_mode||op==7||op==8?0:2));
        printf("operation %d: data/metadata write requests %d, added control requests %d\n",op,writes,events);
        consistent_disk();
        if(reference_mode)continue;
        for(int cut=1;cut<=points;++cut){
            prepare(op);inject=cut;
            if(!setjmp(stopped))operation_run(op);
            return_without_application();++cases;
        }
    }
    if(!reference_mode){
        inject=0;
        for(fail_at=1;fail_at<=6-(fats==1?2:0);++fail_at){prepare(0);operation_run(0);return_without_application();}
        fail_at=0;protocol_cases();
    }
    printf("PASS %u interrupted filesystem boundaries, %u failed writes and 5 protocol refusals\n",cases,reference_mode?0:6-(fats==1?2:0));
    free(marked);free(pristine);free(disk);return 0;
}
