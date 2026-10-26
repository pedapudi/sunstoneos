#ifndef SUNSTONE_SUNXI_SMHC_H
#define SUNSTONE_SUNXI_SMHC_H
/* SMHC v5p3x registers shared by the SD/eMMC core, the SDIO core and the
 * native command filters. Source: A733 user manual revision 1.00, sections
 * 11.3.8 (SMHC0-2) and 11.3.9 (SMHC3). Offsets are relative to a slot base. */
#define SUNXI_SMHC_GCTRL 0x00u
#define SUNXI_SMHC_CLKCR 0x04u
#define SUNXI_SMHC_TMOUT 0x08u
#define SUNXI_SMHC_WIDTH 0x0cu
#define SUNXI_SMHC_BLKSZ 0x10u
#define SUNXI_SMHC_BCNT 0x14u
#define SUNXI_SMHC_CMD 0x18u
#define SUNXI_SMHC_ARG 0x1cu
#define SUNXI_SMHC_RESP0 0x20u
#define SUNXI_SMHC_RESP1 0x24u /* Automatic CMD12 response. */
#define SUNXI_SMHC_IMASK 0x30u
#define SUNXI_SMHC_RINT 0x38u
#define SUNXI_SMHC_STATUS 0x3cu
#define SUNXI_SMHC_FTRGL 0x40u /* FIFO watermarks and IDMAC burst size. */
#define SUNXI_SMHC_CBCR 0x48u /* Bytes moved between card and controller. */
#define SUNXI_SMHC_A12A 0x58u
#define SUNXI_SMHC_NTSR 0x5cu
#define SUNXI_SMHC_DMAC 0x80u
#define SUNXI_SMHC_DLBA 0x84u
#define SUNXI_SMHC_IDST 0x88u
#define SUNXI_SMHC_IDIE 0x8cu
#define SUNXI_SMHC_THLD 0x100u
#define SUNXI_SMHC_EDSD 0x104u
#define SUNXI_SMHC_DRIVE 0x140u
#define SUNXI_SMHC_SAMPLE 0x144u
#define SUNXI_SMHC_FIFO 0x200u
#define SUNXI_SMHC_VERSION 0x300u

/* Command register fields; the low six bits hold the command index. */
#define SUNXI_SMHC_CMD_START (1u << 31)
#define SUNXI_SMHC_CMD_VOLTAGE_SWITCH (1u << 28) /* CMD11 and its clock updates only. */
#define SUNXI_SMHC_CMD_UPDATE_CLOCK (1u << 21)
#define SUNXI_SMHC_CMD_INIT (1u << 15)
#define SUNXI_SMHC_CMD_WAIT (1u << 13)
#define SUNXI_SMHC_CMD_AUTO_STOP (1u << 12)
#define SUNXI_SMHC_CMD_WRITE (1u << 10)
#define SUNXI_SMHC_CMD_DATA (1u << 9)
#define SUNXI_SMHC_CMD_CRC (1u << 8)
#define SUNXI_SMHC_CMD_LONG (1u << 7)
#define SUNXI_SMHC_CMD_RESP (1u << 6)
#define SUNXI_SMHC_CMD_INDEX 63u
#define SUNXI_SMHC_UPDATE (SUNXI_SMHC_CMD_START | SUNXI_SMHC_CMD_UPDATE_CLOCK | SUNXI_SMHC_CMD_WAIT)

/* Raw interrupt status. */
#define SUNXI_SMHC_INT_CMD_DONE (1u << 2)
#define SUNXI_SMHC_INT_DATA_DONE (1u << 3)
#define SUNXI_SMHC_INT_VOLTAGE_DONE (1u << 10) /* 1.8 V switch done; data starvation otherwise. */
#define SUNXI_SMHC_INT_AUTO_STOP_DONE (1u << 14)
#define SUNXI_SMHC_INT_SDIO (1u << 16)
/* Terminal errors of the pinned BSP (SDXC_INTERRUPT_ERROR_BIT): response error
 * 1, response/data CRC 6/7, response/data timeout 8/9, FIFO underrun/overflow
 * 11, command busy 12, start bit 13 and end bit 15. */
#define SUNXI_SMHC_ERRORS 0xbbc2u
/* The SD/eMMC core also stops on data starvation or 1.8 V switch completion
 * (10), write-busy timeout (18) and DAT3 card removal (31). Its profile expects
 * none of them, so each fails closed with retained ownership. SD presence
 * still comes from the board card-detect pin, not from DAT3. */
#define SUNXI_SMHC_MMC_ERRORS (SUNXI_SMHC_ERRORS | (1u << 10) | (1u << 18) | (1u << 31))

/* Status register. FIFO level counts filled 32-bit locations. */
#define SUNXI_SMHC_STATUS_FIFO_EMPTY (1u << 2)
#define SUNXI_SMHC_STATUS_FIFO_FULL (1u << 3)
#define SUNXI_SMHC_STATUS_DAT3 (1u << 8) /* Level of DAT3 (manual 11.3.8.16 CARD_PRESENT). */
#define SUNXI_SMHC_STATUS_BUSY ((1u << 10) | (1u << 9) | (15u << 4)) /* Data FSM, card DAT0, command FSM. */
#define SUNXI_SMHC_STATUS_FIFO_LEVEL(status) (((status) >> 17) & 0x7ffu)
#define SUNXI_SMHC_FIFO_DEPTH 256u /* Words; FIFOTH documents 256 x 32 bits. */

/* Internal DMA (IDMAC), as the board kernel's sunxi-mmc.c and sunxi-mmc.h use
 * it at 2ac08e8c (H:148-149, 254-291). GCTRL bit 31 routes the FIFO to the CPU;
 * IDMAC needs it clear. */
#define SUNXI_SMHC_GCTRL_FIFO_RESET (1u << 1)
#define SUNXI_SMHC_GCTRL_DMA_RESET (1u << 2)
#define SUNXI_SMHC_GCTRL_DMA_ENABLE (1u << 5)
#define SUNXI_SMHC_GCTRL_CPU_FIFO (1u << 31)
#define SUNXI_SMHC_DMAC_RESET (1u << 0)
#define SUNXI_SMHC_DMAC_ON 0x82u /* Fixed burst and IDMAC on (K:958). */
#define SUNXI_SMHC_FTRGL_VALUE 0x200700f8u /* 8-word bursts, receive 7, transmit 248 (V:52). */
#define SUNXI_SMHC_IDST_RECEIVE (1u << 1)
/* Fatal bus error, descriptor unavailable, card error summary, abnormal summary. */
#define SUNXI_SMHC_IDST_ERRORS ((1u << 2) | (1u << 4) | (1u << 5) | (1u << 9))
#define SUNXI_SMHC_IDST_CLEAR 0x337u /* K:1171. */
#define SUNXI_SMHC_IDST_STATE(v) (((v) >> 13) & 15u) /* 0: idle. */
#define SUNXI_SMHC_DES_DIC (1u << 1)
#define SUNXI_SMHC_DES_LD (1u << 2)
#define SUNXI_SMHC_DES_FD (1u << 3)
#define SUNXI_SMHC_DES_CH (1u << 4)
#define SUNXI_SMHC_DES_OWN (1u << 31)
#endif
