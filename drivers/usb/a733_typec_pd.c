/* SPDX-License-Identifier: GPL-2.0-only */
/* FUSB302 framing and PD/VDM fields adapted from Linux TCPM and DisplayPort
 * altmode drivers. Copyright 2015-2017 Google, Inc.
 * Copyright (C) 2018 Intel Corporation
 * Author: Heikki Krogerus <heikki.krogerus@linux.intel.com>
 * DisplayPort is trademark of VESA (www.vesa.org)
 * See docs/typec-pd-sources.json for pinned source and license identities. */
#include "a733_typec.h"
#include <stddef.h>
enum {CAPS,REQUEST,ACCEPT,TRANSITION,PSRDY,IDENT,SVIDS,MODES,ENTER,STATUS,CONFIG,READY,VDM_WAIT};
static uint32_t word(const uint8_t *b)
{return (uint32_t)b[0]|(uint32_t)b[1]<<8|(uint32_t)b[2]<<16|(uint32_t)b[3]<<24;}
/* USB PD uses reflected CRC32 over the little-endian header and objects. */
static uint32_t crc32(const uint8_t *b,unsigned n)
{
    uint32_t crc=UINT32_MAX;
    for(unsigned i=0;i<n;i++){
        crc^=b[i];for(unsigned bit=0;bit<8;bit++)crc=(crc>>1)^((crc&1u)?UINT32_C(0xedb88320):0u);
    }
    return ~crc;
}
static void put(uint8_t *b,uint32_t w)
{for(unsigned i=0;i<4;i++)b[i]=(uint8_t)(w>>(i*8));}
static int limit(uint64_t now,uint64_t delta,uint64_t *out)
{if(now>UINT64_MAX-delta)return -1;*out=now+delta;return 1;}
/* One source-defined I2C FIFO operation, retaining the common engine on error. */
static int io(struct a733_typec *s,unsigned reg,unsigned read,unsigned n)
{
    struct a733_typec_pd *p=&s->pd;int r;
    if(!p->transaction){
        uint64_t d;if(limit(s->now,5000,&d)<0)return -1;
        if(!p->ready&&d>p->deadline)d=p->deadline;
        if(a733_twi_request_block(s->config.bus,0x22,(uint16_t)reg,read,p->data,n,d))return -1;
        p->transaction=1;return 0;
    }
    r=a733_twi_poll(s->config.bus);if(r!=1)return r;
    if(a733_twi_take_block(s->config.bus,read?p->data:NULL,n)!=1)return -1;
    p->transaction=0;return 1;
}
static int send(struct a733_typec *s,unsigned type,unsigned n,const uint32_t *v,unsigned next)
{
    struct a733_typec_pd *p=&s->pd;
    if(p->tx_wait||p->tx_length||n>7)return -1;
    unsigned h=type|0x160u|(p->tx_id<<9)|(n<<12); /* PD2 source/DFP */
    unsigned j=0;p->tx[j++]=0x12;p->tx[j++]=0x12;p->tx[j++]=0x12;p->tx[j++]=0x13;
    p->tx[j++]=(uint8_t)(0x80u|((n*4)+2));p->tx[j++]=(uint8_t)h;p->tx[j++]=(uint8_t)(h>>8);
    for(unsigned i=0;i<n;i++){put(&p->tx[j],v[i]);j+=4;}
    p->tx[j++]=0xff;p->tx[j++]=0x14;p->tx[j++]=0xfe;p->tx[j++]=0xa1;
    p->tx_length=j;p->after_tx=next;return 1;
}
static int vdm(struct a733_typec *s,unsigned command,unsigned sid,unsigned mode,unsigned n,uint32_t arg)
{
    uint32_t v[2]={(sid<<16)|0x8000u|(mode<<8)|command,arg};
    s->pd.command=command;
    return send(s,15,n,v,VDM_WAIT);
}
static int status(struct a733_typec_pd *p,uint32_t v)
{
    /* A sink connection is mandatory. USB/Exit requests terminate this profile. */
    if(!(v&2u)||(v&0x60u)||((v&0x100u)&&!(v&0x80u)))return -1;
    unsigned hpd=!!(v&0x80u);
    /* Count level transitions as well as IRQs so high/low/high cannot make an
     * old token valid again after a downstream reconnect. */
    if((v&0x100u)||(p->ready&&hpd!=p->hpd)){
        if(p->hpd_irq==UINT64_MAX)return -1;
        ++p->hpd_irq;
    }
    p->hpd=hpd;
    return 1;
}
static int received(struct a733_typec *s)
{
    struct a733_typec_pd *p=&s->pd;
    unsigned h=(unsigned)p->rx[1]|(unsigned)p->rx[2]<<8,n=(h>>12)&7,type=h&31,id=(h>>9)&7;
    if(crc32(p->rx+1,2+n*4)!=word(p->rx+3+n*4))return -1;
    if((p->rx[0]&0xe0u)!=0xe0u||(h&0x8000u)||(h&0x120u)||((h>>6)&3)!=1)return -1;
    if(!n&&type==1){ /* FUSB302 puts the peer GoodCRC into the RX FIFO. */
        if(!p->tx_wait||id!=p->tx_id)return -1;
        p->tx_wait=0;p->tx_id=(p->tx_id+1)&7;p->state=p->after_tx;
        if(p->state==TRANSITION&&limit(s->now,35000,&p->transition_until)<0)return -1;
        if(p->state==REQUEST||p->state==VDM_WAIT)return limit(s->now,30000,&p->wait_until);
        return 1;
    }
    if(p->rx_seen&&id==p->rx_id)return 1; /* Already hardware-acknowledged duplicate. */
    if(p->rx_seen&&id!=((p->rx_id+1)&7))return -1;
    p->rx_seen=1;p->rx_id=id;
    if(p->tx_wait)return -1;
    uint32_t v=n?word(p->rx+3):0;
    if(p->state==REQUEST){
        /* Sole fixed PDO, operating and maximum current <= advertised 500mA.
         * GiveBack/reserved fields cannot silently change its interpretation. */
        if(type!=2||n!=1||(v>>28)!=1||(v&0x08f00000u)||
           !((v>>10)&1023)||!((v)&1023)||((v>>10)&1023)>50||(v&1023)>50||
           ((v>>10)&1023)>(v&1023))return -1;
        p->state=ACCEPT;return 1;
    }
    if(type!=15||!n||(v&0x8000u)==0||(v&0x7820u))return -1;
    unsigned sid=v>>16,cmd=v&31,mode=(v>>8)&7,answer=(v>>6)&3;
    if(p->ready&&cmd==6&&answer==0&&sid==0xff01&&mode==p->mode&&n==2)
        return status(p,word(p->rx+7));
    if(p->state!=VDM_WAIT||answer!=1||cmd!=p->command||
       sid!=(cmd<=2?0xff00u:0xff01u)||mode!=(cmd<=3?0:p->mode))return -1;
    switch(cmd){
    case 1:if(n<4||!(word(p->rx+7)&(1u<<26)))return -1;p->state=SVIDS;break;
    case 2:{
        unsigned found=0;for(unsigned i=1;i<n;i++){uint32_t w=word(p->rx+3+i*4);if((w>>16)==0xff01||(w&65535)==0xff01)found=1;}
        if(!found)return -1;
        p->state=MODES;break;}
    case 3:
        for(unsigned i=1;i<n;i++){
            uint32_t w=word(p->rx+3+i*4);unsigned pins=(w>>((w&64)?16:8))&255;
            if((w&1)&&(w&4)&&(pins&0x14)){
                p->mode=i;p->pin_assignment=(pins&4)?4:16;break;
            }
        }
        if(!p->mode)return -1;
        p->state=ENTER;break;
    case 4:if(n!=1)return -1;p->state=STATUS;break;
    case 16:if(n!=2||status(p,word(p->rx+7))<0)return -1;p->state=CONFIG;break;
    case 17:if(n!=1)return -1;p->state=READY;p->ready=1;break;
    default:return -1;
    }
    return 1;
}
static int policy(struct a733_typec *s)
{
    struct a733_typec_pd *p=&s->pd;
    if(p->tx_wait)return s->now>=p->wait_until?-1:1;
    switch(p->state){
    case CAPS:{uint32_t cap=(1u<<26)|(100u<<10)|50u;return send(s,1,1,&cap,REQUEST);}
    case ACCEPT:return send(s,3,0,NULL,TRANSITION);
    case TRANSITION:if(s->now>=p->transition_until)p->state=PSRDY;return 1;
    case PSRDY:return send(s,6,0,NULL,IDENT);
    case IDENT:return vdm(s,1,0xff00,0,1,0);
    case SVIDS:return vdm(s,2,0xff00,0,1,0);
    case MODES:return vdm(s,3,0xff01,0,1,0);
    case ENTER:return vdm(s,4,0xff01,p->mode,1,0);
    case STATUS:return vdm(s,16,0xff01,p->mode,2,1); /* Local DFP_D connection. */
    case CONFIG:return vdm(s,17,0xff01,p->mode,2,(p->pin_assignment<<8)|6u);
    case REQUEST:case VDM_WAIT:return s->now>=p->wait_until?-1:1;
    case READY:return 1;
    default:return -1;
    }
}
int a733_typec_pd_service(struct a733_typec *s)
{
    struct a733_typec_pd *p=&s->pd;int r;
    if(!p->deadline){
        if(limit(s->now,1000000,&p->deadline)<0)return -1;
        if(p->deadline>s->startup_deadline)p->deadline=s->startup_deadline;
        if(!p->deadline)return -1;
    }
    if(!p->ready&&s->now>=p->deadline)return -1;
    if((p->tx_wait||p->state==REQUEST||p->state==VDM_WAIT)&&s->now>=p->wait_until)return -1;
    /* Interrupt masks stay source/CC-only; PD events are polled. AUTO_GCRC
     * acknowledges valid SOP packets in hardware without a scheduling delay. */
    if(p->setup<4){
        static const uint8_t reg[4]={7,6,9,3};
        static const uint8_t val[3]={4,0x44,7}; /* RX/TX FIFO flush, 3 retries */
        if(!p->transaction)p->data[0]=p->setup==3?(uint8_t)(0xb4u|s->polarity):val[p->setup];
        r=io(s,reg[p->setup],0,1);if(r==1)++p->setup;return r;
    }
    if(p->tx_length){
        if(!p->transaction)for(unsigned i=0;i<p->tx_length;i++)p->data[i]=p->tx[i];
        r=io(s,0x43,0,p->tx_length);
        if(r==1){p->tx_length=0;p->tx_wait=1;if(limit(s->now,30000,&p->wait_until)<0)return -1;}
        return r;
    }
    switch(p->phase){
    case 0:
        r=io(s,0x3e,1,1);
        if(r==1){if(p->data[0]&0xbbu)return -1;p->phase=1;}return r;
    case 1:
        r=io(s,0x41,1,1);
        if(r==1){if(p->data[0]&0x14u)return -1;p->phase=(p->data[0]&0x20u)?5:2;}return r;
    case 2:
        r=io(s,0x43,1,3);
        if(r==1){for(unsigned i=0;i<3;i++)p->rx[i]=p->data[i];p->rx_length=4+4*((p->rx[2]>>4)&7);p->phase=3;}return r;
    case 3:
        r=io(s,0x43,1,p->rx_length);
        if(r==1){for(unsigned i=0;i<p->rx_length;i++)p->rx[i+3]=p->data[i];p->phase=4;}return r;
    case 4:r=received(s);p->phase=0;return r;
    case 5:r=policy(s);p->phase=0;return r;
    default:return -1;
    }
}
