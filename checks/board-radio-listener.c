/* Actual native coordinator state machine; lower protocol effects modeled. */
#define main board_radio_unchanged_checkpoint
#include "board-radio.c"
#undef main
#include "../apps/board/application.h"
#include "../apps/board/radio_profile.c"
_Static_assert(sizeof(struct board_radio_profile)==1872&&offsetof(struct board_radio_profile,tcp_mode)==1338,"listener wire geometry");
int main(int argc,char **argv)
{
 if(argc>1)listener_variant=(unsigned)(argv[1][0]-'0');
 /* Variant 4: SEND and RECEIVE time out during a link loss and are repeated. */
 if(listener_variant==4){listener_variant=0;exchange_timeouts=1;}
 struct board_radio_profile raw={.magic=BOARD_RADIO_PROFILE_MAGIC,.version=6,.bytes=1872,.tcp_mode=1,
  .peer_address={10,0,0,1},.peer_port=7400,.client_timeout_ticks=3,.request_bytes=4,.response_bytes=4,
  .request={1,2,3,4},.response={5,6,7,8}};
 struct a733_board_session admitted={.generation=7,.session_start_ticks=24000000,.deadline_us=61000000};
 struct board_radio_profile runtime;uint8_t digest[32];storage_digest_bytes(&raw,sizeof(raw),admitted.radio_profile_digest);
 assert(board_radio_profile_runtime(&runtime,&raw,&admitted,digest)&&runtime.version==5&&runtime.tcp_mode==1);
 assert(runtime.client_deadline_us==admitted.deadline_us-10000&&runtime.platform.handoff==7);
 raw.tcp_mode=0;assert(!board_radio_profile_runtime(&runtime,&raw,&admitted,digest));raw.tcp_mode=1;
 raw.version=4;assert(!board_radio_profile_network_valid(&raw));raw.version=6;
 assert(board_radio_profile_network_valid(&raw));
 app.self=&app;app.constructed=1;app.config.shared=&shared;app.config.recovery_state=&recovery_state;
 app.config.network.clients[0]=task;app.startup_profile=&runtime;atomic_store(&app.network_started,1);client_model=2;
 unsigned iterations=0;int result;
 while((result=board_radio_client_poll(&app))==0)assert(++iterations<30);
 assert(listener_closes==2&&app.listener_handle==5&&app.client_handle==6);
 if(listener_variant)assert(result==-1&&atomic_load(&app.client_status)==2&&app.client_error);
 else assert(result==1&&app.client_sent==4&&app.client_received==4&&atomic_load(&app.client_status)==1);
 if(exchange_timeouts)assert(timed_sends==4&&timed_receives==4);
 puts("PASS board radio listener: authenticated profile, copied partial exchange, repeated timeouts, peer/late/address failure and both closes");
 return 0;
}
