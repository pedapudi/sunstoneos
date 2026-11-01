/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Innolux facts/sequence follow Linux panel-innolux-p079zca.c.
 * Copyright (c) 2017, Fuzhou Rockchip Electronics Co., Ltd
 * LG timing facts follow Linux panel-simple.c (MIT; notice in
 * docs/display-panel-MIT.txt). These descriptions are host examples. */
#include "sunstone_display.h"
static const uint8_t sleep_out[]={0x11},display_on[]={0x29},display_off[]={0x28},sleep_in[]={0x10};
static const struct sunstone_display_mode innolux_mode={56900000,768,1024,40,40,40,20,4,20,0};
static const struct sunstone_panel_step innolux_prepare[]={
 {SUNSTONE_PANEL_GPIO,"enable",0,0,0},{SUNSTONE_PANEL_SUPPLY,"power",1,0,0},
 {SUNSTONE_PANEL_DELAY_US,0,20000,0,0},{SUNSTONE_PANEL_GPIO,"enable",1,0,0},
 {SUNSTONE_PANEL_DELAY_US,0,20000,0,0},{SUNSTONE_PANEL_DCS,0,0,sleep_out,1},
 {SUNSTONE_PANEL_DELAY_US,0,120000,0,0},{SUNSTONE_PANEL_DCS,0,0,display_on,1},
 {SUNSTONE_PANEL_DELAY_US,0,5000,0,0}
};
static const struct sunstone_panel_step innolux_unprepare[]={
 {SUNSTONE_PANEL_DCS,0,0,display_off,1},{SUNSTONE_PANEL_DCS,0,0,sleep_in,1},
 {SUNSTONE_PANEL_GPIO,"enable",0,0,0},{SUNSTONE_PANEL_DELAY_US,0,80000,0,0},
 {SUNSTONE_PANEL_SUPPLY,"power",0,0,0}
};
const struct sunstone_panel sunstone_panel_innolux_p079zca={
 "innolux,p079zca",UINT64_C(0x703037397a6361),&innolux_mode,1,4,
 SUNSTONE_PANEL_SYNC_PULSE|SUNSTONE_PANEL_LP_COMMANDS,SUNSTONE_PANEL_RGB888,
 innolux_prepare,innolux_unprepare,9,5,"backlight",0,0,0
};
static const struct sunstone_display_mode lg_mode={67000000,720,1280,12,4,112,8,4,12,0};
/* The simple-panel driver supplies no panel command table or model delays.
 * Board resources still need a qualified electrical sequence before use. */
static const struct sunstone_panel_step simple_prepare[]={
 {SUNSTONE_PANEL_SUPPLY,"power",1,0,0},{SUNSTONE_PANEL_GPIO,"enable",1,0,0}
};
static const struct sunstone_panel_step simple_unprepare[]={
 {SUNSTONE_PANEL_GPIO,"enable",0,0,0},{SUNSTONE_PANEL_SUPPLY,"power",0,0,0}
};
const struct sunstone_panel sunstone_panel_lg_lh500wx1_sd03={
 "lg,lh500wx1-sd03",UINT64_C(0x6c676c68353030),&lg_mode,1,4,0,SUNSTONE_PANEL_RGB888,
 simple_prepare,simple_unprepare,2,2,"backlight",0,0,0
};
