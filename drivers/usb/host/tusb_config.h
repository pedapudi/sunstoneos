/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_USB2_HOST_TUSB_CONFIG_H
#define SUNSTONE_USB2_HOST_TUSB_CONFIG_H
#define CFG_TUSB_MCU OPT_MCU_NONE
/* The actual FreeRTOS host task is the sole stack caller/producer. No USB ISR
 * or second task accesses this stack; its queue therefore needs no OS lock. */
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUSB_DEBUG 0
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_HOST | OPT_MODE_HIGH_SPEED)
#define CFG_TUH_ENABLED 1
#define CFG_TUD_ENABLED 0
#define CFG_TUH_DEVICE_MAX 1
#define CFG_TUH_HUB 0
#define CFG_TUH_MSC 1
#define CFG_TUH_MSC_MAXLUN 1
#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_TASK_QUEUE_SZ 8
#define CFG_TUH_TASK_EVENTS_PER_RUN 2
#define CFG_TUH_CONTROL_TIMEOUT_MS 1000
#define CFG_TUH_MEM_ALIGN __attribute__((aligned(64)))
#define CFG_TUH_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(64)))
#endif
