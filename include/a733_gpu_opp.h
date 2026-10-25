#ifndef SUNSTONE_A733_GPU_OPP_H
#define SUNSTONE_A733_GPU_OPP_H
#include <stdint.h>
/* The GPU operating point SunstoneOS runs, from the vendor device tree's
 * gpu_opp_table (arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi:4736-4760 at
 * kernel 2ac08e8c7, opp@1008000000): the GPU core at 1008 MHz from PLL_GPU0,
 * DCDC4 (gpu-supply) at the node's opp-microvolt-vfdefault, 960 mV. The
 * vendor kernel's device tree has no vf_mapping_table, so its sunxi_get_opp
 * (BXM DDK rgx_sunxi/sunxi_platform.c) takes vfdefault on every board; the
 * u-boot tree maps speed-bin codes 0, 1 and 2 to vf0000, vf0100 and vf0200,
 * which are 960 mV as well. docs/designs/gpu-vendor-default.md. */
#define A733_GPU_CORE_HZ 1008000000u
#define A733_GPU_SUPPLY_UV 960000u
/* DCDC4 selectors (AXP8191 register 0x15, 7-bit field, 10 mV steps from
 * 500 mV): the power-on 800 mV and the operating point's 960 mV. */
#define A733_GPU_DCDC4_BOOT 0x1eu
#define A733_GPU_DCDC4_SELECTOR 0x2eu
#define A733_GPU_BOOT_UV 800000u

/* 1 when uv is the 1008 MHz voltage of vfdefault or of one of the node's 21
 * speed bins (860-960 mV), the voltages the vendor runs this operating point
 * at. The PMIC provider admits a DCDC4 write only to one of them. */
static inline int a733_gpu_opp_voltage(uint32_t uv)
{
    /* opp-microvolt-vf<code> of opp@1008000000, in the node's order;
     * code 0xffff stands for opp-microvolt-vfdefault. */
    static const struct { uint16_t code; uint32_t uv; } bins[22] = {
        {0xffff, 960000}, {0x0000, 960000}, {0x0100, 960000}, {0x0101, 920000}, {0x0200, 960000},
        {0x0201, 880000}, {0x0202, 920000}, {0x0302, 920000}, {0x0402, 900000}, {0x0502, 860000},
        {0x0602, 860000}, {0x0300, 940000}, {0x0400, 900000}, {0x0401, 880000}, {0x0500, 880000},
        {0x0501, 860000}, {0x0503, 860000}, {0x0600, 920000}, {0x0700, 880000}, {0x0800, 940000},
        {0x1400, 880000}, {0x1500, 860000}};
    for (unsigned i = 0; i < sizeof(bins) / sizeof(*bins); ++i)
        if (bins[i].uv == uv) return 1;
    return 0;
}
#endif
