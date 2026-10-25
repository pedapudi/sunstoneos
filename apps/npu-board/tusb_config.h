#ifndef SUNSTONE_NPU_BOARD_TUSB_CONFIG_H
#define SUNSTONE_NPU_BOARD_TUSB_CONFIG_H
/* Private configuration for the NPU board image. The dedicated hardware-FIFO
 * path transfers directly between the MUSB FIFO and each vendor class FIFO,
 * removing the endpoint staging copy. The 4 KiB transmit FIFO admits the
 * catalogue's largest READ response (2,816 payload bytes plus 64 header bytes)
 * before the USB task handles completion. */
#define CFG_TUD_EDPT_DEDICATED_HWFIFO 1
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
