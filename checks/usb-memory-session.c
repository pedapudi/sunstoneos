/* Exercise the production geometry validator and its extent reader. The
 * complete session digest and board admission have separate checks. */
#include "../apps/os/session.c"
#include <assert.h>
#include <stdio.h>
void __assert_func(const char *file,int line,const char *function,const char *expression)
{(void)file;(void)line;(void)function;(void)expression;__builtin_trap();}
static _Alignas(8) unsigned char record[SS_SESSION_MAX_BYTES];
int main(void)
{
    struct ss_session_header *h=(void *)(record+SS_SESSION_PREFIX_BYTES);
    *h=(struct ss_session_header){.magic=SS_SESSION_MAGIC,.version=SS_SESSION_VERSION,
        .bytes=SS_SESSION_PREFIX_BYTES+sizeof(*h)};
    assert(usb_memory_extent_valid(record));
    struct ss_extent *e=(void *)(h+1);
    *e=(struct ss_extent){.type=SS_EXTENT_USB_MEMORY,.bytes=sizeof(struct ss_usb_memory_extent)};
    struct ss_usb_memory_extent *m=(void *)(e+1);
    *m=(struct ss_usb_memory_extent){.version=1,.base=A733_USB_MEMORY_BASE,
        .bytes=A733_USB_MEMORY_BYTES,.alias=A733_USB_MEMORY_ALIAS_BASE};
    h->bytes+=sizeof(*e)+sizeof(*m);h->extents=1;
    assert(usb_memory_extent_valid(record)); /* No fabricated evidence digest. */
    uint64_t *fields[]={&m->version,&m->base,&m->bytes,&m->alias};
    for(unsigned i=0;i<4;++i){*fields[i]^=1;assert(!usb_memory_extent_valid(record));*fields[i]^=1;}
    e->bytes-=8;h->bytes-=8;assert(!usb_memory_extent_valid(record));
    puts("USB session optional geometry accepts zero evidence and refuses mismatched bounds");
    return 0;
}
