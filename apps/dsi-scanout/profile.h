#ifndef SUNSTONE_DSI_PROFILE_H
#define SUNSTONE_DSI_PROFILE_H
#include "a733_board_application.h"
#include "a733_display_native.h"
#include "a733_dsi.h"
#include "a733_pins.h"
#include "a733_rail_leases.h"
#define DSI_SESSION_BYTES 4096u
#define DSI_PROFILE_MAGIC UINT64_C(0x3153494433375341)
#define DSI_STEPS 48u
#define DSI_PAYLOAD 1536u
enum { DSI_PIXEL,DSI_HS,DSI_LS,DSI_WIDTH,DSI_HEIGHT,DSI_HSYNC,DSI_HBACK,DSI_HFRONT,DSI_VSYNC,DSI_VBACK,DSI_VFRONT,DSI_LANES,DSI_FORMAT,DSI_MODE,DSI_DCLK,DSI_PITCH,DSI_INITIAL_HS,DSI_INITIAL_LS,DSI_PLL_WORD0,DSI_TIMEOUT=25,DSI_SEED,DSI_PADDING,DSI_STEP_COUNT,DSI_PAYLOAD_BYTES,DSI_PIN_COUNT,DSI_SUPPLY_COUNT };
enum { DSI_PIN_LANE,DSI_PIN_POWER,DSI_PIN_RESET,DSI_PIN_BACKLIGHT };
enum { DSI_STEP_GPIO=1,DSI_STEP_DELAY,DSI_STEP_PACKET };
struct dsi_pin {uint8_t bank,pin,expected[3],desired[3],output,role,reserved[6];};
struct dsi_step {uint8_t kind,role,type,phase;uint16_t offset,bytes;uint32_t value,reserved2;};
struct dsi_supply {uint32_t rail,uv;};
struct dsi_session {
 struct a733_board_session board;
 uint64_t magic,version,bytes,owner,board_id,panel_id,authority,pll_profile_id,pll_profile_generation,
 reference_generation,rail_generation,inactive_epoch,recovery_generation,pck_client;
 uint8_t facts[4][32]; /* initial isolation, full memory, electrical/PLL, selected panel */
 uint32_t values[32];struct dsi_supply supplies[8];struct dsi_pin pins[16];
 struct dsi_step steps[DSI_STEPS];uint8_t payload[DSI_PAYLOAD],reserved[512];
};
_Static_assert(sizeof(struct dsi_session)==4096,"DSI sealed page");
_Static_assert(offsetof(struct dsi_session,values)==832&&offsetof(struct dsi_session,payload)==2048,"DSI wire offsets");
int dsi_profile_valid(const struct dsi_session *);
int dsi_request(const struct dsi_session *,struct display_request *,uint64_t frame_pa,uintptr_t frame_cpu);
uint64_t dsi_frame_bytes(const struct dsi_session *);
#endif
