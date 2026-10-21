/* Print every PCIe register write of the nominal core and endpoint-test flows.
 * The driver uses only interfaces that predate link-speed selection, so the
 * same file builds against an earlier revision for a byte-for-byte comparison.
 * Controller, endpoint and scheduler behavior are the existing models.
 * a733-pcie-trace.py supplies checks/a733-pcie-epf.c with its main renamed. */
#include "a733-pcie-epf-entry.c"
static int (*inner_write)(void *,uintptr_t,unsigned,uint32_t);
static int logged_write(void *v,uintptr_t a,unsigned width,uint32_t value)
{
 printf("%08lx %u %08x\n",(unsigned long)a,width,value);
 return inner_write(v,a,width,value);
}
static void log_writes(void){inner_write=port.ops.write;port.ops.write=logged_write;}
int main(int argc,char **argv)
{
 assert(argc==2);
 assert(mmap((void *)0x41c00000,2*1024*1024,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
 if(!strcmp(argv[1],"core")){
  setup();log_writes();assert(complete()==A733_PCIE_READY);
  uint32_t value=0;
  assert(a733_pcie_config_read(&port,0,0,model.now+10000,&value)==A733_PCIE_READY&&value==0x56781234);
  assert(a733_pcie_config_read(&port,0,8,model.now+10000,&value)==A733_PCIE_READY);
  return 0;
 }
 assert(!strcmp(argv[1],"nominal")||!strcmp(argv[1],"wide"));
 begin(!strcmp(argv[1],"wide"),0);log_writes();
 wait_command();endpoint_complete();assert(advance()==A733_PCIE_READY);
 assert(a733_pcie_epf_copy(&client,output)==A733_PCIE_READY&&client.published);
 assert(advance()==A733_PCIE_READY);
 return 0;
}
