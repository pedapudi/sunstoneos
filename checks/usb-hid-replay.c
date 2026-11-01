#include "sunstone_usb_hid.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned events;
static void emit(void *ctx,const struct sunstone_hid_event *e)
{
 (void)ctx;if(events++)putchar(',');
 printf("[%u,%u,%d,%d,%d,%d]",e->type,e->usage,e->value,e->x,e->y,e->wheel);
}
static unsigned word(FILE *f)
{unsigned char v[2];if(fread(v,1,2,f)!=2)exit(2);return v[0]|(unsigned)v[1]<<8;}
int main(int argc,char **argv)
{
 if(argc!=2)return 2;FILE *f=fopen(argv[1],"rb");if(!f)return 2;
 uint8_t descriptor[SUNSTONE_HID_DESCRIPTOR_BYTES],report[SUNSTONE_HID_REPORT_BYTES];
 unsigned bytes=word(f),reports=word(f);if(bytes>sizeof descriptor||fread(descriptor,1,bytes,f)!=bytes)return 2;
 struct sunstone_usb_hid h={0};putchar('[');
 if(!sunstone_usb_hid_mount(&h,2,0,11,descriptor,bytes,emit,NULL))return 3;
 for(unsigned i=0;i<reports;++i){bytes=word(f);if(bytes>sizeof report||fread(report,1,bytes,f)!=bytes)return 2;
  if(sunstone_usb_hid_report(&h,11,report,bytes)!=1)return 4;}
 if(!sunstone_usb_hid_unmount(&h,11)||fgetc(f)!=EOF)return 5;
 puts("]");fclose(f);return 0;
}
