#define main incoming_regression_main
#include "bt-acl-incoming.c"
#undef main
#include "bt_security.h"
#include <stdlib.h>
TaskHandle_t xTaskGetCurrentTaskHandle(void){return (TaskHandle_t)(uintptr_t)1;}
static void setup(struct fixture *f,struct bt_acl *a,struct bt_security *s,atomic_uint *recovery)
{
    init(f);
    const unsigned ops[]={HCI_OP_RESET,HCI_OP_READ_LOCAL_VERSION,HCI_OP_READ_LOCAL_COMMANDS,
        HCI_OP_READ_LOCAL_FEATURES,HCI_OP_READ_BUFFER_SIZE,HCI_OP_READ_BD_ADDR};
    const unsigned sizes[]={1,9,65,9,8,7};
    for(unsigned i=0;i<6;++i){
        send(f,ops[i]);
        if(i==3){uint8_t p[12]={1,3,16,0};p[10]=8;event(f,HCI_EV_CMD_COMPLETE,p,sizeof(p));}
        else cc(f,ops[i],1,sizes[i],0);
        for(unsigned j=0;j<4 && f->hci.stage==i;++j)step(f,64);
    }
    memset(a,0,sizeof(*a));assert(bt_acl_init(a,&f->hci)==1);
    struct bt_security_policy policy={.peer={1,2,3,4,5,6},.allow_just_works=1,.minimum_level=2,.deadline=10000};
    memset(s,0,sizeof(*s));atomic_init(recovery,0);
    assert(bt_security_init(s,a,xTaskGetCurrentTaskHandle(),recovery,&policy,500)==1);
}
static void configured(struct fixture *f,struct bt_acl *a,struct bt_security *s)
{
    for(unsigned pass=0;!s->configured && pass<12;++pass){
        assert(bt_security_service(s,10000)==1);if(!s->pending)continue;
        astep(a);unsigned op=f->hci.opcode;
        if(op==0x0c55){uint8_t p[]={1,0x55,0x0c,0,1};event(f,HCI_EV_CMD_COMPLETE,p,sizeof(p));}
        else if(op==0x1004){uint8_t p[14]={1,4,16,0};event(f,HCI_EV_CMD_COMPLETE,p,sizeof(p));}
        else cc(f,op,1,1,0);
        astep(a);
    }
    assert(s->configured && !s->count && !s->pending);
}
int main(int argc,char **argv)
{
    (void)incoming_regression_main;assert(argc==2);unsigned which=(unsigned)atoi(argv[1]);
    struct fixture f;struct bt_acl a;struct bt_security s;atomic_uint recovery;
    setup(&f,&a,&s,&recovery);
    if(which==0){
        f.current=0;assert(bt_security_stop(&s,3000)==1);f.current=1;
        assert(bt_security_service(&s,3000)==1 && !s.count && !s.pending && !s.configured);
        assert(bt_acl_stop(&a,3000)==1 && bt_security_quiescent(&s)==1);
    }else if(which==1 || which==2){
        assert(bt_security_service(&s,10000)==1 && s.pending);astep(&a);
        uint64_t original=s.command_deadline;assert(bt_security_stop(&s,3000)==1);
        assert(bt_security_stop(&s,5000)==1 && s.deadline==3000 && s.command_deadline==original);
        assert(bt_security_quiescent(&s)==0);
        if(which==2){f.now=original;assert(bt_security_step(&s,64,3000)==-1 && bt_security_quiescent(&s)==-1);}
        else{
            cc(&f,0x0c56,1,1,0);astep(&a);assert(bt_security_service(&s,3000)==1);
            assert(s.configuration_stage==1 && !s.configured && !s.count && !s.pending);
            assert(bt_acl_stop(&a,3000)==1 && bt_security_quiescent(&s)==1);
        }
    }else if(which==5){
        configured(&f,&a,&s);uint64_t gen=begin_connect(&f,&a,1);
        assert(bt_security_attach(&s,gen)==1);
        cs(&f,HCI_OP_CREATE_CONN,1,0x0c);astep(&a);assert(a.links[0].state==BT_ACL_CLOSED);
        struct bt_acl_peer peer={{1,2,3,4,5,6},1,0,0x8123};uint64_t replacement=0;
        assert(bt_acl_connect(&a,&peer,5000,&replacement)==1 && replacement!=gen);
        assert(bt_security_stop(&s,3000)==1 && bt_acl_stop(&a,3000)==1);
        assert(bt_acl_quiescent(&a)==1 && bt_security_quiescent(&s)==-1);
    }else{
        configured(&f,&a,&s);uint64_t gen=begin_connect(&f,&a,1);
        assert(bt_security_attach(&s,gen)==1);connected(&f,&a,gen,0x23);
        uint8_t key[23]={1,2,3,4,5,6};for(unsigned i=6;i<22;++i)key[i]=(uint8_t)i;key[22]=4;
        event(&f,0x18,key,sizeof(key));astep(&a);
        struct bt_keys_link_key *saved=s.connections[0].br.link_key;assert(saved);
        uint8_t retained_key[16];memcpy(retained_key,saved->val,16);
        assert(bt_security_start(&s,gen)==1 && bt_security_service(&s,10000)==1);astep(&a);
        assert(f.hci.opcode==0x0411);cs(&f,0x0411,1,0);astep(&a);
        assert(bt_security_stop(&s,3000)==1 && bt_security_start(&s,gen)==-1);
        if(which==4){event(&f,0x31,key,6);assert(bt_acl_step(&a,64,3000)==-1 && bt_security_quiescent(&s)==-1);}
        else{
            uint8_t authenticated[]={0,0x23,0};event(&f,0x06,authenticated,sizeof(authenticated));astep(&a);
            assert(s.connections[0].authenticated && !s.count && !s.pending);
            assert(bt_acl_stop(&a,3000)==1);astep(&a);astep(&a);
            cs(&f,HCI_OP_DISCONNECT,1,0);astep(&a);disconnect_event(&f,0x23);astep(&a);
            assert(bt_acl_quiescent(&a)==1 && bt_security_quiescent(&s)==1);
            assert(!s.connections[0].authenticated && !s.connections[0].encrypt);
            assert(s.connections[0].br.link_key==saved && !memcmp(saved->val,retained_key,16));
        }
    }
    printf("Bluetooth security return case %u passed.\n",which);return 0;
}
