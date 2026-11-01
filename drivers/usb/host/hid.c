#include "sunstone_usb_hid.h"
#include <string.h>
#include <limits.h>

struct globals {
    uint32_t page, size, count, id;
    int64_t min, max;
};
struct locals {
    uint32_t usage[SUNSTONE_HID_USAGES], min, max;
    unsigned count, range;
};
static int64_t signed_item(uint32_t v,unsigned bytes)
{
    if(bytes&&bytes<4&&(v&(1u<<(bytes*8-1))))return (int64_t)v-(1LL<<(bytes*8));
    return bytes==4?(int64_t)(int32_t)v:v;
}
static uint32_t usage_value(uint32_t v,unsigned bytes,uint32_t page)
{return bytes==4?v:(page<<16)|v;}
static uint32_t local_usage(const struct locals *l,unsigned i)
{
    if(l->range)return l->min+(i<=l->max-l->min?i:l->max-l->min);
    return l->count?l->usage[i<l->count?i:l->count-1]:0;
}
static void event(struct sunstone_usb_hid *h,unsigned type,unsigned id,unsigned usage,
                  int value,int x,int y,int wheel)
{
    struct sunstone_hid_event e={.generation=h->generation,.address=h->address,
        .instance=h->instance,.type=(uint8_t)type,.report_id=(uint8_t)id,
        .usage=(uint16_t)usage,.value=value,.x=x,.y=y,.wheel=wheel};
    h->emit(h->context,&e);
}
static int parse(struct sunstone_usb_hid *h,const uint8_t *p,size_t bytes)
{
    struct globals g={0},stack[4];struct locals l={0};
    unsigned depth=0,collections=0,kinds[8]={0},kind=0;
    for(size_t at=0;at<bytes;){
        unsigned prefix=p[at++],n=prefix&3u,type=(prefix>>2)&3u,tag=prefix>>4;
        if(prefix==0xfe)return 0; /* Long items have no supported semantics. */
        if(n==3)n=4;
        if(n>bytes-at)return 0;
        uint32_t v=0;for(unsigned i=0;i<n;++i)v|=(uint32_t)p[at++]<<(8*i);
        if(type==1){
            switch(tag){
            case 0:if(v>65535)return 0;g.page=v;break;
            case 1:g.min=signed_item(v,n);break;
            case 2:g.max=g.min<0?signed_item(v,n):v;break;
            case 7:g.size=v;break;
            case 8:if(!v||v>255)return 0;g.id=v;h->numbered=1;break;
            case 9:g.count=v;break;
            case 10:if(n||depth==4)return 0;stack[depth++]=g;break;
            case 11:if(n||!depth)return 0;g=stack[--depth];break;
            case 3:case 4:case 5:case 6:break; /* Physical range and units. */
            default:return 0;
            }
        }else if(type==2){
            if(tag==0){if(l.range||l.count==SUNSTONE_HID_USAGES)return 0;
                l.usage[l.count++]=usage_value(v,n,g.page);
            }else if(tag==1){if(l.count||l.range)return 0;l.min=usage_value(v,n,g.page);l.range=1;
            }else if(tag==2){if(l.range!=1)return 0;l.max=usage_value(v,n,g.page);
                if(l.max<l.min||(l.max>>16)!=(l.min>>16))return 0;l.range=2;
            }else if(tag==10)return 0; /* Delimited alternate usages unsupported. */
            else if(tag<3||tag>9)return 0;
        }else if(type==0){
            if(l.range==1)return 0;
            if(tag==10){
                if(!n||collections==8)return 0;kinds[collections++]=kind;
                if(v==1){uint32_t u=local_usage(&l,0);kind=u==0x10006?1:u==0x10002?2:0;}
            }else if(tag==12){if(n||!collections)return 0;kind=kinds[--collections];
            }else if(tag==8){
                if(!n||!collections||!g.size||g.size>32||!g.count||g.count>512||g.min>g.max)return 0;
                unsigned r=0;while(r<h->reports&&h->report[r].id!=g.id)++r;
                if(r==h->reports){if(r==SUNSTONE_HID_REPORTS)return 0;h->report[r].id=(uint8_t)g.id;++h->reports;}
                struct sunstone_hid_report *report=&h->report[r];
                uint64_t bits=(uint64_t)g.size*g.count+report->bits;
                if(bits>8*(SUNSTONE_HID_REPORT_BYTES-(g.id?1:0)))return 0;
                if(!(v&1)&&kind){
                    if((v&~0x7fu)||h->fields==SUNSTONE_HID_FIELDS)return 0;
                    struct sunstone_hid_field *f=&h->field[h->fields++];
                    f->bit=report->bits;f->size=(uint8_t)g.size;f->count=(uint16_t)g.count;
                    f->report=(uint8_t)r;f->flags=(uint8_t)v;f->kind=(uint8_t)kind;
                    f->logical_min=g.min;f->logical_max=g.max;
                    f->usages=(uint8_t)l.count;memcpy(f->usage,l.usage,sizeof(l.usage));
                    f->usage_min=l.range?l.min:0;f->usage_max=l.range?l.max:0;
                    if(!(v&2)&&kind!=1)return 0; /* Mouse arrays unsupported. */
                    for(unsigned j=0;j<g.count;++j){
                        uint32_t u=local_usage(&l,j),page=u>>16;
                        if(kind==1&&page==7)h->capabilities|=SUNSTONE_HID_KEYBOARD;
                        if(kind==2&&(page==9||(page==1&&((u&65535)==0x30||(u&65535)==0x31||(u&65535)==0x38))))
                            h->capabilities|=SUNSTONE_HID_POINTER;
                    }
                }
                report->bits=(uint16_t)bits;
            }else if(tag!=9&&tag!=11)return 0;
            memset(&l,0,sizeof(l));
        }else return 0;
    }
    if(collections||depth||!h->fields||!h->capabilities)return 0;
    for(unsigned i=0;i<h->reports;++i)if(h->numbered&&!h->report[i].id)return 0;
    return 1;
}
int sunstone_usb_hid_mount(struct sunstone_usb_hid *h,uint8_t address,uint8_t instance,
    uint64_t generation,const uint8_t *descriptor,size_t bytes,sunstone_hid_emit emit,void *context)
{
    if(!h||h->mounted||!address||address>127||!generation||!descriptor||!bytes||
       bytes>SUNSTONE_HID_DESCRIPTOR_BYTES||!emit)return 0;
    memset(h,0,sizeof(*h));
    if(!parse(h,descriptor,bytes)){memset(h,0,sizeof(*h));return 0;}
    h->generation=generation;h->address=address;h->instance=instance;
    h->emit=emit;h->context=context;h->mounted=1;event(h,SUNSTONE_HID_ATTACH,0,h->capabilities,0,0,0,0);return 1;
}
static uint32_t field_usage(const struct sunstone_hid_field *f,uint32_t index)
{
    if(f->usages)return f->usage[index<f->usages?index:f->usages-1];
    return f->usage_min?f->usage_min+(index<=f->usage_max-f->usage_min?index:f->usage_max-f->usage_min):0;
}
static int bit(const uint8_t *p,unsigned n){return (p[n/8]>>(n%8))&1;}
static int64_t read_field(const uint8_t *p,unsigned offset,unsigned size,int sign)
{
    uint32_t value=0;for(unsigned i=0;i<size;++i)value|=(uint32_t)bit(p,offset+i)<<i;
    return sign&&(value&(1u<<(size-1)))?(int64_t)value-(1LL<<size):value;
}
static void union_state(const struct sunstone_usb_hid *h,uint8_t keys[32],uint32_t *buttons)
{
    memset(keys,0,32);*buttons=0;
    for(unsigned r=0;r<h->reports;++r){
        for(unsigned b=0;b<32;++b)keys[b]|=h->report[r].keys[b];
        *buttons|=h->report[r].buttons;
    }
}
int sunstone_usb_hid_report(struct sunstone_usb_hid *h,uint64_t generation,const uint8_t *p,size_t bytes)
{
    if(!h||!h->mounted||generation!=h->generation||!p||!bytes)return 0;
    unsigned id=h->numbered?*p++:0,r=0;if(h->numbered)--bytes;
    while(r<h->reports&&h->report[r].id!=id)++r;
    if(r==h->reports||bytes!=(h->report[r].bits+7u)/8)return 0;
    uint8_t keys[32]={0},before[32],after[32];uint32_t buttons=0,old_buttons,new_buttons;
    int64_t x=0,y=0,wheel=0;
    for(unsigned i=0;i<h->fields;++i){
        const struct sunstone_hid_field *f=&h->field[i];if(f->report!=r)continue;
        for(unsigned j=0;j<f->count;++j){
            int64_t v=read_field(p,f->bit+j*f->size,f->size,f->logical_min<0);
            if(v<f->logical_min||v>f->logical_max){if(f->flags&0x40)continue;return 0;}
            uint32_t u;
            if(f->flags&2)u=field_usage(f,j);
            else {
                if(!v)continue;
                uint32_t index=(uint32_t)(v-f->logical_min);
                if((f->usages&&index>=f->usages)||(!f->usages&&index>f->usage_max-f->usage_min)){
                    if(f->flags&0x40)continue;
                    return 0;
                }
                u=field_usage(f,index);
            }
            unsigned page=u>>16,usage=u&65535;
            if(f->kind==1&&page==7){
                if(!(f->flags&2)&&usage>=1&&usage<=3)return 2;
                if(usage>3&&usage<256&&v)keys[usage/8]|=(uint8_t)(1u<<(usage%8));
            }else if(f->kind==2&&page==9&&usage&&usage<=32){if(v)buttons|=1u<<(usage-1);
            }else if(f->kind==2&&page==1&&(f->flags&6)==6){
                if(usage==0x30)x+=v;else if(usage==0x31)y+=v;else if(usage==0x38)wheel+=v;
            }
        }
    }
    if(x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX||wheel<INT32_MIN||wheel>INT32_MAX)return 0;
    union_state(h,before,&old_buttons);memcpy(h->report[r].keys,keys,32);h->report[r].buttons=buttons;
    union_state(h,after,&new_buttons);
    for(unsigned u=4;u<256;++u)if(bit(before,u)!=bit(after,u))event(h,SUNSTONE_HID_KEY,id,u,bit(after,u),0,0,0);
    for(unsigned u=0;u<32;++u)if(((old_buttons^new_buttons)>>u)&1)event(h,SUNSTONE_HID_BUTTON,id,u+1,(new_buttons>>u)&1,0,0,0);
    if(x||y||wheel)event(h,SUNSTONE_HID_MOTION,id,0,0,(int)x,(int)y,(int)wheel);
    return 1;
}
int sunstone_usb_hid_unmount(struct sunstone_usb_hid *h,uint64_t generation)
{
    if(!h||!h->mounted||h->generation!=generation)return 0;
    uint8_t keys[32];uint32_t buttons;union_state(h,keys,&buttons);
    for(unsigned u=4;u<256;++u)if(bit(keys,u))event(h,SUNSTONE_HID_KEY,0,u,0,0,0,0);
    for(unsigned u=0;u<32;++u)if((buttons>>u)&1)event(h,SUNSTONE_HID_BUTTON,0,u+1,0,0,0,0);
    event(h,SUNSTONE_HID_DETACH,0,h->capabilities,0,0,0,0);memset(h,0,sizeof(*h));return 1;
}
