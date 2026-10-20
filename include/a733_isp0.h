/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SUNSTONE_A733_ISP0_H
#define SUNSTONE_A733_ISP0_H
#include <stdint.h>
#include <stddef.h>
#define A733_ISP0_INPUT_IOVA UINT32_C(0x00100000)
#define A733_ISP0_LOAD_IOVA UINT32_C(0x00200000)
#define A733_ISP0_SAVE_IOVA UINT32_C(0x00210000)
#define A733_ISP0_SAVE_LOAD_IOVA UINT32_C(0x00220000)
#define A733_ISP0_VIPP_IOVA UINT32_C(0x00230000)
#define A733_ISP0_Y_IOVA UINT32_C(0x00300000)
#define A733_ISP0_UV_IOVA UINT32_C(0x00380000)
#define A733_ISP0_INPUT_BYTES 614400u
#define A733_ISP0_INPUT_MAP 618496u
#define A733_ISP0_PLANE_BYTES 307200u
#define A733_ISP0_LOAD_BYTES 0x7700u
#define A733_ISP0_SAVE_BYTES 0x6a00u
#define A733_ISP0_SAVE_LOAD_BYTES 0x3400u
/* Fixed ISP600 channel0, BGGR12 in LE16 containers -> 640x480 YUV422.
 * All optional processing/3A modules are disabled; mandatory Bayer conversion
 * retains the pinned complete default coefficient image. Table/stat/save-load
 * DMA remains mapped. No release, drain or sensor/PHY configuration exists. */
struct a733_isp0_ops {
 int (*now)(void *,uint64_t *);
 int (*read)(void *,uintptr_t,uint32_t *);
 int (*write)(void *,uintptr_t,uint32_t);
};
struct a733_isp0 {
 struct a733_isp0_ops ops;void *context;
 uint64_t deadline,last_now;
 uint32_t isp_status,vipp_status,pattern_status;
 unsigned initialized,index,phase,armed,started,fault;
};
int a733_isp0_parameters(void *load,size_t,void *save_load,size_t,void *vipp,size_t);
int a733_isp0_init(struct a733_isp0 *,const struct a733_isp0_ops *,void *,uint64_t);
/* Bounded configuration under exclusive VI/ISP0/TOP/VIPP0 ownership and
 * actual master1+2 mappings. Each step performs at most three ordered accesses.
 * Producer is quiet until trigger, after the downstream DMA is armed. */
int a733_isp0_poll(struct a733_isp0 *);
int a733_isp0_trigger(struct a733_isp0 *);
/* Fresh latched ISP parameter save/load, VIPP load and one-shot START clear.
 * Caller independently requires DMA CAPTURE_DONE, fault scan and guarded copy.
 * This never proves transaction drain or permits any backing reuse. */
int a733_isp0_complete(struct a733_isp0 *);
#endif
