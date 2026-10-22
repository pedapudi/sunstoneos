/* Print the register words of the NVMe host model (a733-pcie-nvme.c) once its
 * port is READY with the cold NVMe function attached, so that the linked-image
 * check starts from the same modeled state. Every value is a software model.
 * checks/pcie-nvme-native.py generates the included copy of that model with
 * its main renamed to nvme_main. */
#include "a733-pcie-nvme-entry.c"
int main(void)
{
    assert(mmap((void *)(uintptr_t)MEMORY,2*1024*1024,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=MAP_FAILED);
    device();
    printf("root_cap %02x\nphase %u\n",port.root_cap,port.phase);
    for(unsigned i=0;i<model.word_count;++i)
        printf("%08lx %08x\n",(unsigned long)model.words[i].address,model.words[i].value);
    return 0;
}
