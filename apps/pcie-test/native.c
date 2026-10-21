#include "pcie.h"
#include "boot.h"
#include "aarch64_device_memory.h"
#include "a733_memory_uncached.h"
#include "a733_storage_digest.h"
#include <string.h>
struct pcie_application pcie;
static struct device_memory_pool *pool;
struct device_memory_pool *pcie_test_pool(void){return pool;}
int pcie_live(void)
{return board_live(NULL)&&!atomic_load_explicit(&pcie.failed,memory_order_acquire)&&
 !atomic_load_explicit(&providers.shared.clocks.failed,memory_order_acquire)&&
 !atomic_load_explicit(&providers.shared.locks.retained,memory_order_acquire)&&
 a733_smp_counter()/24<board.session.deadline_us-10000;}
void pcie_cancel(void)
{atomic_store_explicit(&pcie.failed,1,memory_order_release);atomic_store_explicit(&pcie.result,2,memory_order_release);}
int pcie_output_ready(void)
{return pcie_live()&&atomic_load_explicit(&pcie.result,memory_order_acquire)==1&&
 a733_smp_counter()/24<atomic_load_explicit(&pcie.observed_until,memory_order_acquire);}
static int separate(const void *a,size_t n,const void *b,size_t m)
{uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;return a&&b&&n<=UINTPTR_MAX-x&&m<=UINTPTR_MAX-y&&(x+n<=y||y+m<=x);}
static int mappings(void)
{
 const uintptr_t ranges[][2]={{0x22000000,0x22100000},{0x02000000,0x02004000},{0x03400000,0x03401000},{0x06000000,0x06001000},
 {0x06300000,0x06302000},{0x06400000,0x06401000},{0x06c00000,0x06c03000},
 {0x06c06000,0x06c07000},{0x06ca0000,0x06cc0000},{0x07010000,0x07011000},{0x07067000,0x07068000},
 {0x07090000,0x07091000},{0x20000000,0x20001000}};
 for(unsigned i=0;i<sizeof(ranges)/sizeof(*ranges);++i)for(uintptr_t a=ranges[i][0];a<ranges[i][1];a+=4096){
  struct aarch64_translation t;if(!aarch64_device_translate(a,1,&t)||t.physical!=a||t.attributes!=0||t.shareability!=2)return 0;
 }
 return 1;
}
static int admitted(void *v,const struct a733_pcie_config *c)
{
 return v==&pcie&&pcie_live()&&c==&pcie.port.config&&c->owner==pcie.session.owner&&
 c->identity==pcie.session.authority&&c->topology==pcie.session.board.topology&&
 c->deadline==pcie.config.deadline&&c->lifetime_deadline==pcie.config.lifetime_deadline&&
 c->pck_client==pcie.session.pck_client&&c->link_speed==1&&!memcmp(c->inherited,pcie.config.inherited,sizeof(c->inherited))&&
 !memcmp(c->fabric,board.session.fabric,sizeof(c->fabric))&&!memcmp(c->fabric_hz,board.session.fabric_hz,sizeof(c->fabric_hz))&&mappings();
}
static int now(void *v,uint64_t *n)
{return v==&pcie&&pcie_live()&&pcie.native.operations.now(&pcie.native,n);}
static int current(void *v)
{return v==&pcie&&pcie_live()&&pcie.native.operations.current(&pcie.native);}
static int admit(void *v,const struct a733_pcie_config *c)
{return v==&pcie&&pcie_live()?pcie.native.operations.admit(&pcie.native,c):-1;}
static int enter(void *v,uint64_t d)
{return v==&pcie&&pcie_live()?pcie.native.operations.enter(&pcie.native,d):-1;}
static void leave(void *v)
{if(v==&pcie)pcie.native.operations.leave(&pcie.native);}
static void retain(void *v)
{if(v==&pcie)pcie.native.operations.retain(&pcie.native);}
static int read_register(void *v,uintptr_t a,unsigned width,uint32_t *out)
{return v==&pcie&&pcie_live()&&pcie.native.operations.read(&pcie.native,a,width,out)==1&&pcie_live();}
static int write_register(void *v,uintptr_t a,unsigned width,uint32_t value)
{return v==&pcie&&pcie_live()&&pcie.native.operations.write(&pcie.native,a,width,value)==1&&pcie_live();}
static int outside(void *v,const void *p,size_t n)
{return v==&pcie&&pcie.native.operations.outside(&pcie.native,p,n)&&
 separate(p,n,&providers,sizeof(providers))&&separate(p,n,&pcie.session,sizeof(pcie.session));}
int pcie_construct(void)
{
 if(!board_live(NULL)||board.session.deadline_us<=10000||!pcie.handle||!mappings())return 0;
 pool=a733_memory_uncached_init();if(!pool)return 0;
 uint64_t start=board.session.session_start_ticks/24;
 if(start>UINT64_MAX-pcie.session.link_timeout_us||start+pcie.session.link_timeout_us>=board.session.deadline_us-10000)return 0;
 atomic_init(&pcie.failed,0);atomic_init(&pcie.result,0);atomic_init(&pcie.observed_until,0);
 pcie.config=(struct a733_pcie_config){.owner=pcie.session.owner,.identity=pcie.session.authority,
 .topology=pcie.session.board.topology,.deadline=start+pcie.session.link_timeout_us,
 .lifetime_deadline=board.session.deadline_us-10000,.pck_client=(unsigned)pcie.session.pck_client,.link_speed=1};
 for(unsigned i=0;i<4;++i)pcie.config.inherited[i]=(struct a733_pin_config){pcie.session.inherited[i][0],pcie.session.inherited[i][1],pcie.session.inherited[i][2]};
 memcpy(pcie.config.fabric,board.session.fabric,sizeof(pcie.config.fabric));memcpy(pcie.config.fabric_hz,board.session.fabric_hz,sizeof(pcie.config.fabric_hz));
 if(!a733_pcie_native_init(&pcie.native,&pcie.port,&providers.shared.locks,pcie.handle,admitted,&pcie))return 0;
 pcie.operations=(struct a733_pcie_ops){now,current,admit,enter,leave,retain,read_register,write_register,outside};
 storage_digest_bytes(&pcie.session,sizeof(pcie.session),pcie.session_digest);
 return a733_pcie_init(&pcie.port,&pcie.config,&pcie.operations,&pcie,&providers.shared.clocks,
 &providers.shared.pins,&providers.shared.rails,&providers.shared.pmic,&providers.shared.gpu);
}
