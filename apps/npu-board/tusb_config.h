#ifndef SUNSTONE_NPU_BOARD_TUSB_CONFIG_H
#define SUNSTONE_NPU_BOARD_TUSB_CONFIG_H
/* TinyUSB configuration of the NPU board image. apps/npu-board comes first in
 * the image's include path, so this file replaces the board's storage-update
 * configuration for every source of the image. It differs from that file only
 * in the vendor transmit FIFO, 4 KiB instead of 2 KiB, so that a READ response
 * of the catalogue's largest output (2,816 bytes plus the 64-byte header)
 * enters the empty FIFO in one write (apps/npu-board/usb.c). The 2 KiB
 * increase is .bss. checks/npu-session-runtime.c, case 5, models USB0 with
 * this file. */
#define CFG_TUSB_MCU OPT_MCU_NONE
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUSB_DEBUG 0
#define CFG_TUSB_DEBUG_BREAKPOINT a733_usb_assert
#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_HIGH_SPEED
#define TUP_DCD_ENDPOINT_MAX 6
#define TUP_RHPORT_HIGHSPEED 1
#define CFG_TUD_ENDPOINT0_SIZE 64
#define CFG_TUD_ENDPPOINT_MAX 3
#define CFG_TUD_VENDOR 1
#define CFG_TUD_VENDOR_RX_BUFSIZE 2048
#define CFG_TUD_VENDOR_TX_BUFSIZE 4096
#define CFG_TUD_VENDOR_RX_EPSIZE 512
#define CFG_TUD_VENDOR_TX_EPSIZE 512
#define CFG_TUD_TASK_EVENTS_PER_RUN 4
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(64)))
#endif
