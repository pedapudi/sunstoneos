/* Actual DSI application/producers; reuse the accepted initial wire fixtures. */
#define DSI_NATIVE_REUSE
#define vTaskDelay unused_static_scanout_delay
#define a733_display_native_read previous_display_read
#define a733_display_native_write previous_display_write
#include "dsi-scanout-native.c"
#undef a733_display_native_write
#undef a733_display_native_read
#undef vTaskDelay
#include "../apps/dsi-scanout/input.h"
#include "a733_storage_digest.h"
static uint8_t *uploads[2],present_headers[3][96];static size_t upload_bytes[2],upload_at[2];
static unsigned upload_slot,completed,replay_fault,rcq_pending,rcq_copied,frame_pending;
static const char *output_directory;
static uintptr_t translated(uint64_t iova,size_t bytes)
{
 uint64_t pa;assert(a733_iommu_translate(&dsi.iommu.tables,iova,A733_IOMMU_READ,&pa)==1);
 assert(pa>=dsi.frame_extent.physical&&bytes<=dsi.frame_extent.bytes&&pa-dsi.frame_extent.physical<=dsi.frame_extent.bytes-bytes);
 return dsi.frame_extent.cpu+(uintptr_t)(pa-dsi.frame_extent.physical);
}
int a733_display_native_read(uintptr_t address,uint32_t *out)
{
 if(address==A733_DE_BASE+0x8144&&dsi.native.binding.scanout.de.action==A733_DE_REPLAY){
  struct a733_de *de=&dsi.native.binding.scanout.de;
  if(de->step==9)*word(address)|=16;
  if(de->step==13&&rcq_pending){
   assert(!rcq_copied);rcq_copied=1;rcq_pending=0;
   uint64_t header=*word(A733_DE_BASE+0x8154);assert(!*word(A733_DE_BASE+0x8158)&&*word(A733_DE_BASE+0x815c)==32);
   const uint32_t *h=(void *)translated(header,32);assert(h[1]==4&&h[2]==1&&h[3]==0x221010&&!h[4]&&!h[5]&&!h[6]&&!h[7]);
   *word(A733_DE_BASE+h[3])=*(uint32_t *)translated(h[0],4);*word(address)|=4;
   if(replay_fault==1)*word(A733_DE_BASE+h[3])^=4;
  }
  if(de->step==18&&frame_pending){*word(address)|=1;frame_pending=0;}
 }
 return previous_display_read(address,out);
}
int a733_display_native_write(uintptr_t address,uint32_t value)
{
 uint32_t old=*word(address);int r=previous_display_write(address,value);
 if(address==A733_DE_BASE+0x8144){*word(address)=old&~(value&13u);if(dsi.native.binding.scanout.de.step==16)frame_pending=1;}
 if(address==A733_DE_BASE+0x8150&&(value&1)){rcq_pending=1;rcq_copied=0;*word(address)=0;}
 return r;
}
int a733_display_native_rcq_submit(void)
{
 assert(pm.register_held&&!pm.retained);*word(A733_DE_BASE+0x8144)|=16;
 uint32_t status,control;assert(a733_display_native_read(A733_DE_BASE+0x8144,&status));if(!(status&16))return 0;
 assert(a733_display_native_read(A733_DE_BASE+0x8150,&control));return a733_display_native_write(A733_DE_BASE+0x8150,control|1);
}
void vTaskDelay(TickType_t ticks)
{
 assert(ticks==1);pm.now+=1000;
 if(atomic_load_explicit(&dsi.input.allocated,memory_order_acquire)&&upload_slot<2){
  size_t n=dsi_input_remaining(&dsi.input);if(n>512)n=512;if(n>upload_bytes[upload_slot]-upload_at[upload_slot])n=upload_bytes[upload_slot]-upload_at[upload_slot];
  int r=dsi_input_feed(&dsi.input,&dsi.session,dsi_input_now,&dsi,uploads[upload_slot]+upload_at[upload_slot],n);assert(r>=0);
  upload_at[upload_slot]+=n;if(upload_at[upload_slot]==upload_bytes[upload_slot])++upload_slot;
 }
 if(upload_slot==2&&!atomic_load_explicit(&dsi.input.request,memory_order_acquire)&&completed<3){
  assert(dsi_input_feed(&dsi.input,&dsi.session,dsi_input_now,&dsi,present_headers[completed],96)==1);
 }
 if(atomic_load_explicit(&dsi.input.request,memory_order_acquire)==3){
  char name[1024];assert(snprintf(name,sizeof(name),"%s/present-%u.bin",output_directory,completed+1)>0);
  FILE *f=fopen(name,"wb");assert(f&&fwrite(dsi.replay_result,1,256,f)==256&&!fclose(f));
  ++completed;atomic_store_explicit(&dsi.input.request,0,memory_order_release);
  if(completed==3)longjmp(done,1);
 }
 if(atomic_load_explicit(&dsi.result,memory_order_acquire)==2)longjmp(done,1);
 pm.worker=1;enum a733_pmic_status r=a733_pmic_poll(&providers.shared.pmic);pm.worker=0;assert(r!=A733_PMIC_RETAINED);
 assert(pm.now<60000000);
}
static uint8_t *read_file(const char *path,size_t *bytes)
{FILE *f=fopen(path,"rb");assert(f&&!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0&&!fseek(f,0,SEEK_SET));uint8_t *p=malloc((size_t)n);assert(p&&fread(p,1,(size_t)n,f)==(size_t)n&&!fclose(f));*bytes=(size_t)n;return p;}
int main(int argc,char **argv)
{
 assert(argc==9);size_t n;uint8_t *s=read_file(argv[1],&n);assert(n==4096);memcpy(&input,s,4096);free(s);assert(input.version==2&&dsi_profile_valid(&input));
 for(unsigned i=0;i<2;++i)uploads[i]=read_file(argv[2+i],&upload_bytes[i]);
 for(unsigned i=0;i<3;++i){uint8_t *p=read_file(argv[4+i],&n);assert(n==96);memcpy(present_headers[i],p,96);free(p);}
 output_directory=argv[7];if(!strcmp(argv[8],"readback"))replay_fault=1;else assert(!strcmp(argv[8],"success"));
 board.admitted_session=input.board;board.session=input.board;board.session.session_start_ticks=0;board.session.deadline_us=60000000;
 application_setup();if(!setjmp(done))dsi_task(0);
 if(replay_fault){assert(atomic_load(&dsi.result)==2&&completed==0);assert(device_memory_lookup(&memory_pool,dsi.frame)->state==DEVICE_MEMORY_RETAINED&&device_memory_lookup(&memory_pool,dsi.tables)->state==DEVICE_MEMORY_RETAINED);}
 else {assert(completed==3&&dsi.iommu.state==A733_IOMMU_ACTIVE);uint8_t hash[32];
 for(unsigned i=0;i<2;++i){storage_digest_bytes((void *)(dsi.frame_extent.cpu+i*dsi_frame_bytes(&input)),dsi_frame_bytes(&input),hash);assert(!memcmp(hash,input.replay.hashes[i],32));}
 assert(a733_de_replay_valid((void *)(dsi.frame_extent.cpu+2*dsi_frame_bytes(&input)),0x10000000,dsi_frame_bytes(&input)));}
 printf("PASS actual %ux%u two-upload/three-replay native composition %s; immutable bank retained; no retirement\n",input.values[3],input.values[4],argv[8]);
 for(unsigned i=0;i<2;++i)free(uploads[i]);free(backing);return 0;
}
