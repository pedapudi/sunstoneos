/* SPDX-License-Identifier: GPL-2.0-only */
#include "sunstone_board.h"
#include "camera_sensor.h"
#include "sunstone_display.h"
#include "a733_pins.h"
#include "a733_twi.h"
#include "a733_rail_leases.h"

/* V1.2 schematic sheets 13 and 17, recorded in docs/board-wiring.md.
 * The reference camera module uses two of each connector's four lanes.
 * Module supplies and optional RESET wiring require a session-bound module
 * description: UINT16_MAX denotes an unspecified supply, never rail zero. */
static const struct camera_sensor_bus cameras[] = {
    {.port=0,.parser=0,.lanes=2,.virtual_channel=0,
     .i2c_controller=A733_TWI_MAIN11,.i2c_address=0x3c,.mclk=1,.xclk_hz=24000000,
     .pwdn={A733_PIN_E,6,1,1},.reset={0,0,0,0},
     .csi={{A733_PIN_K,0,2},{A733_PIN_K,1,2},{A733_PIN_K,2,2},{A733_PIN_K,3,2},
           {A733_PIN_K,4,2},{A733_PIN_K,5,2},{A733_PIN_K,6,2},{A733_PIN_K,7,2},
           {A733_PIN_K,8,2},{A733_PIN_K,9,2}},
     .mclk_pin={A733_PIN_E,5,2},.i2c_scl={A733_PIN_E,7,8},.i2c_sda={A733_PIN_E,8,8},
     .regulator={UINT16_MAX,UINT16_MAX,UINT16_MAX},
     .power_hold_us=5000,.reset_hold_us=1000,.bus_hold_us=20000},
    {.port=1,.parser=1,.lanes=2,.virtual_channel=0,
     .i2c_controller=A733_TWI_MAIN9,.i2c_address=0x3c,.mclk=2,.xclk_hz=24000000,
     .pwdn={A733_PIN_E,10,1,1},.reset={0,0,0,0},
     .csi={{A733_PIN_K,10,2},{A733_PIN_K,11,2},{A733_PIN_K,12,2},{A733_PIN_K,13,2},
           {A733_PIN_K,14,2},{A733_PIN_K,15,2},{A733_PIN_K,16,2},{A733_PIN_K,17,2},
           {A733_PIN_K,18,2},{A733_PIN_K,19,2}},
     .mclk_pin={A733_PIN_E,9,2},.i2c_scl={A733_PIN_E,14,8},.i2c_sda={A733_PIN_E,15,8},
     .regulator={UINT16_MAX,UINT16_MAX,UINT16_MAX},
     .power_hold_us=5000,.reset_hold_us=1000,.bus_hold_us=20000}
};
static const char *const sensors[] = {"ov5640"};
/* NULL panels describe the unpopulated connector, not a selected accessory. */
static const struct sunstone_display_port displays[] = {
    {.name="mini-HDMI",.connector=SUNSTONE_DISPLAY_HDMI,.controller=0},
    {.name="TYPEC24",.connector=SUNSTONE_DISPLAY_DP,.controller=0,
     .lanes=4,.lane_map={0,1,2,3},.resource_id=1},
    {.name="LCD",.connector=SUNSTONE_DISPLAY_DSI,.controller=1,
     .lanes=4,.lane_map={0,1,2,3}}
};
static const struct sunstone_usb_port usb[] = {
    {.name="TYPEC24",.controller=2,.typec_i2c_controller=A733_TWI_R1,
     .typec_address=0x22,.vbus_bank=A733_PIN_L,.vbus_pin=8,.vbus_active_high=1,
     .irq_bank=A733_PIN_L,.irq_pin=9,.scl_bank=A733_PIN_L,.scl_pin=12,
     .sda_bank=A733_PIN_L,.sda_pin=13,.i2c_function=3,
     .supplies={A733_RAIL_DCDC1,A733_RAIL_DC1SW2},.supply_uv=3300000}
};
static const struct sunstone_board_description board = {
    .name="Orange Pi Zero 3W V1.2",
    .camera=cameras,.camera_count=sizeof(cameras)/sizeof(cameras[0]),
    .camera_sensors=sensors,.camera_sensor_count=sizeof(sensors)/sizeof(sensors[0]),
    .display_ports=displays,.display_port_count=sizeof(displays)/sizeof(displays[0]),
    .usb_ports=usb,.usb_port_count=sizeof(usb)/sizeof(usb[0])
};
const struct sunstone_board_description *sunstone_board_orangepi_zero3w(void)
{ return &board; }
