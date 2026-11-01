#include "sunstone_usb_hid.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
/* Authored protocol fixtures. Public recorded descriptors are loaded separately. */
static const uint8_t keyboard[]={
 5,1,9,6,0xa1,1,5,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,
 0x75,1,0x95,8,0x81,2,0x75,8,0x95,1,0x81,1,
 0x19,0,0x29,0xff,0x15,0,0x26,0xff,0,0x75,8,0x95,6,0x81,0,0xc0};
static const uint8_t mouse[]={
 5,1,9,2,0xa1,1,5,9,0x19,1,0x29,3,0x15,0,0x25,1,
 0x75,1,0x95,3,0x81,2,0x75,5,0x95,1,0x81,1,
 5,1,9,0x30,9,0x31,9,0x38,0x15,0x81,0x25,0x7f,
 0x75,8,0x95,3,0x81,6,0xc0};
static struct sunstone_hid_event events[2048];static unsigned count;
static void emit(void *ctx,const struct sunstone_hid_event *e)
{assert(ctx==(void *)42);assert(count<2048);events[count++]=*e;}
static int mount(struct sunstone_usb_hid *h,const uint8_t *d,size_t n)
{return sunstone_usb_hid_mount(h,2,0,11,d,n,emit,(void *)42);}
static void expect(unsigned n,unsigned type,unsigned usage,int value)
{assert(n<count);assert(events[n].generation==11&&events[n].address==2);assert(events[n].type==type&&events[n].usage==usage&&events[n].value==value);}
static void basic(void)
{
 struct sunstone_usb_hid h={0};uint8_t p[9]={2,0,4};count=0;
 assert(mount(&h,keyboard,sizeof keyboard));assert(!mount(&h,keyboard,sizeof keyboard));
 assert(sunstone_usb_hid_report(&h,11,p,8)==1);assert(count==3);
 expect(0,SUNSTONE_HID_ATTACH,1,0);expect(1,SUNSTONE_HID_KEY,4,1);expect(2,SUNSTONE_HID_KEY,225,1);
 assert(sunstone_usb_hid_report(&h,11,p,8)==1&&count==3); /* No typematic duplicates. */
 assert(!sunstone_usb_hid_report(&h,12,p,8));assert(!sunstone_usb_hid_report(&h,11,p,7));
 assert(!sunstone_usb_hid_report(&h,11,p,9));assert(!sunstone_usb_hid_unmount(&h,12));
 p[2]=1;assert(sunstone_usb_hid_report(&h,11,p,8)==2&&count==3);
 assert(sunstone_usb_hid_unmount(&h,11));assert(count==6);
 expect(3,SUNSTONE_HID_KEY,4,0);expect(4,SUNSTONE_HID_KEY,225,0);expect(5,SUNSTONE_HID_DETACH,1,0);
 assert(!sunstone_usb_hid_report(&h,11,p,8));assert(!sunstone_usb_hid_unmount(&h,11));
 count=0;assert(mount(&h,mouse,sizeof mouse));uint8_t m[]={5,0xff,2,0xff};
 assert(sunstone_usb_hid_report(&h,11,m,4)==1&&count==4);
 expect(1,SUNSTONE_HID_BUTTON,1,1);expect(2,SUNSTONE_HID_BUTTON,3,1);
 assert(events[3].type==SUNSTONE_HID_MOTION&&events[3].x==-1&&events[3].y==2&&events[3].wheel==-1);
 m[1]=0x80;assert(!sunstone_usb_hid_report(&h,11,m,4)&&count==4);
 assert(sunstone_usb_hid_unmount(&h,11)&&count==7);expect(4,SUNSTONE_HID_BUTTON,1,0);expect(5,SUNSTONE_HID_BUTTON,3,0);
}
static void combined(void)
{
 /* Two independent top-level collections, numbered reports. */
 uint8_t d[sizeof keyboard+sizeof mouse+4];size_t n=0;
 d[n++]=0x85;d[n++]=1;memcpy(d+n,keyboard,sizeof keyboard);n+=sizeof keyboard;
 d[n++]=0x85;d[n++]=2;memcpy(d+n,mouse,sizeof mouse);n+=sizeof mouse;
 struct sunstone_usb_hid h={0};count=0;assert(mount(&h,d,n));
 uint8_t k[]={1,0,0,4,0,0,0,0,0},m[]={2,1,3,0,0};
 assert(sunstone_usb_hid_report(&h,11,k,sizeof k)==1);assert(sunstone_usb_hid_report(&h,11,m,sizeof m)==1);
 expect(1,SUNSTONE_HID_KEY,4,1);expect(2,SUNSTONE_HID_BUTTON,1,1);assert(events[3].x==3);
 memset(k+1,0,8);assert(sunstone_usb_hid_report(&h,11,k,sizeof k)==1);expect(4,SUNSTONE_HID_KEY,4,0);
 m[0]=9;assert(!sunstone_usb_hid_report(&h,11,m,sizeof m)&&count==5);
 assert(sunstone_usb_hid_unmount(&h,11));expect(5,SUNSTONE_HID_BUTTON,1,0);
 /* Same key held by two IDs must release only after both release. */
 memcpy(d+2+sizeof keyboard+2,keyboard,sizeof keyboard);n=sizeof keyboard*2+4;
 count=0;assert(mount(&h,d,n));k[3]=4;k[0]=1;assert(sunstone_usb_hid_report(&h,11,k,sizeof k)==1);
 k[0]=2;assert(sunstone_usb_hid_report(&h,11,k,sizeof k)==1&&count==2);
 k[3]=0;k[0]=1;assert(sunstone_usb_hid_report(&h,11,k,sizeof k)==1&&count==2);
 k[0]=2;assert(sunstone_usb_hid_report(&h,11,k,sizeof k)==1&&count==3);expect(2,SUNSTONE_HID_KEY,4,0);
}
static void bitmap_and_boot_mouse(void)
{
 const uint8_t nkro[]={5,1,9,6,0xa1,1,5,7,0x19,0,0x29,0xff,0x15,0,0x25,1,0x75,1,0x96,0,1,0x81,2,0xc0};
 struct sunstone_usb_hid h={0};uint8_t p[32]={0};count=0;assert(mount(&h,nkro,sizeof nkro));
 p[4/8]|=1u<<(4%8);p[225/8]|=1u<<(225%8);
 assert(sunstone_usb_hid_report(&h,11,p,sizeof p)==1);expect(1,SUNSTONE_HID_KEY,4,1);expect(2,SUNSTONE_HID_KEY,225,1);
 assert(sunstone_usb_hid_unmount(&h,11));
 /* Three-byte boot mouse, without wheel, still uses Report protocol. */
 uint8_t d[sizeof mouse];size_t n=0;
 for(size_t i=0;i<sizeof mouse;++i){
  if(i+1<sizeof mouse&&mouse[i]==9&&mouse[i+1]==0x38){++i;continue;}
  d[n++]=mouse[i];
 }
 for(size_t i=0;i+3<n;++i)if(d[i]==0x95&&d[i+1]==3&&d[i+2]==0x81&&d[i+3]==6)d[i+1]=2;
 count=0;assert(mount(&h,d,n));uint8_t m[]={1,0xff,2};
 assert(sunstone_usb_hid_report(&h,11,m,sizeof m)==1);expect(1,SUNSTONE_HID_BUTTON,1,1);
 assert(events[2].x==-1&&events[2].y==2&&events[2].wheel==0);assert(sunstone_usb_hid_unmount(&h,11));
}
static void malformed(void)
{
 struct sunstone_usb_hid h={0};count=0;
 for(size_t n=0;n<sizeof keyboard;++n)assert(!mount(&h,keyboard,n));
 for(size_t n=0;n<sizeof mouse;++n)assert(!mount(&h,mouse,n));
 uint8_t d[1100];memset(d,0,sizeof d);assert(!mount(&h,d,sizeof d));
 const uint8_t cases[][8]={{0xfe,0,0},{0xb4},{0xa4,0xa4,0xa4,0xa4,0xa4},{0x85,0},{0x09},{0xc0},{0x05,1,0x39,1}};
 const size_t len[]={3,1,5,2,1,1,4};
 for(unsigned i=0;i<sizeof len/sizeof *len;++i)assert(!mount(&h,cases[i],len[i]));
 memcpy(d,keyboard,sizeof keyboard);d[17]=33;assert(!mount(&h,d,sizeof keyboard)); /* Width overflow. */
 /* Mixed numbered/unnumbered reports. */
 memcpy(d,keyboard,sizeof keyboard);d[sizeof keyboard]=0x85;d[sizeof keyboard+1]=1;
 memcpy(d+sizeof keyboard+2,keyboard,sizeof keyboard);assert(!mount(&h,d,sizeof keyboard*2+2));
 assert(count==0);
}
static void fuzz(void)
{
 /* Deterministic malformed inputs under ASan/UBSan, with mounted-report faults. */
 uint32_t rng=12345;uint8_t d[160],p[80];struct sunstone_usb_hid h={0};
 for(unsigned i=0;i<10000;++i){
  for(unsigned j=0;j<sizeof d;++j){rng=rng*1664525+1013904223;d[j]=(uint8_t)(rng>>24);}
  count=0;if(mount(&h,d,rng%sizeof d))sunstone_usb_hid_unmount(&h,11);
 }
 for(unsigned i=0;i<1000;++i){count=0;assert(mount(&h,keyboard,sizeof keyboard));
  for(unsigned j=0;j<sizeof p;++j){rng=rng*1664525+1013904223;p[j]=(uint8_t)(rng>>24);}
  sunstone_usb_hid_report(&h,11,p,rng%sizeof p);assert(sunstone_usb_hid_unmount(&h,11));
 }
}
int main(void){basic();combined();bitmap_and_boot_mouse();malformed();fuzz();puts("HID state, combined reports, descriptor faults and sanitizer fuzz passed");return 0;}
