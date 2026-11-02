/* Maps the session address for tools/host_admission.c; built without the image's definitions or include paths. */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
void *host_map_session(uintptr_t address, size_t bytes)
{
    void *at = mmap((void *)address, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return at == MAP_FAILED ? NULL : at;
}
