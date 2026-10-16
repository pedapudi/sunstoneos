#include "FreeRTOS.h"
#include "task.h"
#include "platform.h"
#include "boot.h"
#include "a733_usb.h"
#include "usb_service.h"
#include "tusb.h"
#include <string.h>

#define MESSAGE_MAGIC 0x53534f53U
#define BODY_MAX 2048U
static StaticTask_t task_state;
static StackType_t task_stack[2048];
static uint8_t request[16 + BODY_MAX] __attribute__((aligned(64)));
static uint8_t response[16 + BODY_MAX] __attribute__((aligned(64)));
static uint32_t received, response_size, sent;
static uint64_t commands;
static bool recover_requested;
static volatile bool response_complete;
static TickType_t last_command;
static volatile uint64_t last_progress;

uint64_t usb_runtime_last_progress(void) { return last_progress; }

__attribute__((weak)) bool usb_application_dispatch(uint32_t command, const uint8_t *input,
                                                   uint32_t size, uint8_t *output, unsigned *length)
{
    (void)command; (void)input; (void)size; (void)output; (void)length;
    return false;
}

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bDeviceClass = 0, .bDeviceSubClass = 0,
    .bDeviceProtocol = 0, .bMaxPacketSize0 = 64,
    // RAM diagnostics retain the board's recovery ID. Product/bcdDevice and
    // protocol identify this runtime separately from the ROM FEL service.
    .idVendor = 0x1f3a, .idProduct = 0xefe8, .bcdDevice = 0x0100,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1
};
static const uint8_t config_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(0, 4, 0x02, 0x81, 512)
};
static const uint8_t config_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN, 0, 100),
    TUD_VENDOR_DESCRIPTOR(0, 4, 0x02, 0x81, 64)
};
static const tusb_desc_device_qualifier_t qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER, .bcdUSB = 0x0200,
    .bDeviceClass = 0, .bDeviceSubClass = 0, .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64, .bNumConfigurations = 1, .bReserved = 0
};
uint8_t const *tud_descriptor_device_cb(void) { return (const uint8_t *)&device_descriptor; }
uint8_t const *tud_descriptor_device_qualifier_cb(void) { return (const uint8_t *)&qualifier; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    a733_usb_trace(5, index, tud_speed_get());
    return tud_speed_get() == TUSB_SPEED_HIGH ? config_hs : config_fs;
}
uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
    (void)index;
    static uint8_t other[sizeof(config_hs)];
    memcpy(other, tud_speed_get() == TUSB_SPEED_HIGH ? config_fs : config_hs, sizeof(other));
    other[1] = TUSB_DESC_OTHER_SPEED_CONFIG;
    return other;
}
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t language)
{
    (void)language;
    static uint16_t descriptor[64];
    static const char *const strings[] = {"", "sunstoneos", "sunstoneos RAM diagnostics",
                                         "sunstoneos-a733-ram", "Diagnostics"};
    if (index == 0) {
        descriptor[0] = (TUSB_DESC_STRING << 8) | 4;
        descriptor[1] = 0x0409;
        return descriptor;
    }
    if (index >= sizeof(strings) / sizeof(strings[0])) return NULL;
    unsigned count = (unsigned)strlen(strings[index]);
    if (count > 63) count = 63;
    descriptor[0] = (TUSB_DESC_STRING << 8) | (2 * count + 2);
    for (unsigned i = 0; i < count; ++i) descriptor[i + 1] = strings[index][i];
    return descriptor;
}
void tud_mount_cb(void) { platform_puts("USB configured\n"); }
void tud_umount_cb(void)
{
    received = response_size = sent = 0;
    recover_requested = false;
    platform_puts("USB unmounted\n");
}
void tud_vendor_tx_cb(uint8_t interface, uint32_t transferred)
{
    (void)interface;
    a733_usb_trace(11, transferred, sent);
    if (sent == response_size && tud_vendor_write_available() == CFG_TUD_VENDOR_TX_BUFSIZE)
        response_complete = true;
}

static void renew_recovery_timer(void)
{
    register uint64_t operation __asm__("x0") = 0x734;
    __asm__ volatile("smc #0" : "+r"(operation) :: "memory");
    configASSERT(operation == 0);
}

static void dispatch(void)
{
    uint32_t *in = (uint32_t *)request;
    uint32_t *out = (uint32_t *)response;
    unsigned length = 0;
    memcpy(response, request, 16);
    out[2] |= 0x80000000U;
    ++commands;
    last_command = xTaskGetTickCount();
    a733_usb_trace(10, in[2], in[3]);
    switch (in[2]) {
    case 1: {
        uint64_t status[5];
        status[0] = xTaskGetTickCount();
        __asm__ volatile("mrs %0, CurrentEL" : "=r"(status[1]));
        status[2] = commands;
        status[3] = tud_speed_get();
        status[4] = counter();
        length = sizeof(status);
        memcpy(response + 16, status, length);
        break;
    }
    case 2:
        length = in[3];
        memcpy(response + 16, request + 16, length);
        break;
    case 3:
        length = (unsigned)RESULT->log_length;
        if (length > BODY_MAX) length = BODY_MAX;
        memcpy(response + 16, (const void *)(RESULT_BASE + LOG_OFFSET), length);
        break;
    case 4:
        recover_requested = true;
        break;
    default:
        if (!usb_application_dispatch(in[2], request + 16, in[3], response + 16, &length))
            out[2] = 0xffffffffU;
        break;
    }
    configASSERT(length <= BODY_MAX);
    out[3] = length;
    response_size = length + 16;
    sent = 0;
    response_complete = false;
    received = 0;
}

static void usb_task(void *argument)
{
    (void)argument;
    renew_recovery_timer();
    tusb_rhport_init_t init = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_HIGH};
    configASSERT(tusb_init(0, &init));
    platform_puts("USB runtime ready\n");
    TickType_t start = xTaskGetTickCount();
    last_command = start;
    for (;;) {
        renew_recovery_timer();
        a733_usb_poll();
        tud_task_ext(0, false);
        if (response_size) {
            if (sent < response_size) {
                sent += tud_vendor_write(response + sent, response_size - sent);
                tud_vendor_write_flush();
            }
            if (response_complete) {
                response_size = 0;
                if (recover_requested) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    tud_disconnect();
                    vTaskDelay(pdMS_TO_TICKS(150));
                    platform_puts("PASS USB: host requested recovery\n");
                    platform_exit(0);
                }
            }
        } else if (tud_mounted() && tud_vendor_available()) {
            unsigned needed = 16;
            if (received >= 16) {
                uint32_t *header = (uint32_t *)request;
                if (header[0] != MESSAGE_MAGIC || header[3] > BODY_MAX)
                    platform_panic("invalid USB message", header[3]);
                needed += header[3];
            }
            if (received < needed)
                received += tud_vendor_read(request + received, needed - received);
            if (received >= 16) {
                uint32_t *header = (uint32_t *)request;
                if (header[0] != MESSAGE_MAGIC || header[3] > BODY_MAX)
                    platform_panic("invalid USB message", header[3]);
                if (received == 16 + header[3]) dispatch();
            }
        }
        // Mode 2 bounds an unattended bring-up attempt; mode 3 stays resident.
        if (RESULT->mode == 2 && xTaskGetTickCount() - start > pdMS_TO_TICKS(8000))
            platform_panic("USB host check deadline", 0);
        // Application sessions recover when their host stops sending commands.
        if (RESULT->mode == 4 && xTaskGetTickCount() - last_command > pdMS_TO_TICKS(30000))
            platform_panic("USB host lease expired", 0);
        last_progress = counter();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void usb_runtime_start(unsigned priority)
{
    last_progress = counter();
    configASSERT(xTaskCreateStatic(usb_task, "usb", 2048, NULL, priority, task_stack, &task_state));
}

#ifndef SUNSTONE_MIMUS_USB
void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    platform_panic(name, 0);
}
void kernel_main(void)
{
    platform_init();
    platform_puts("sunstoneos FreeRTOS USB diagnostics\n");
    usb_runtime_start(3);
    vTaskStartScheduler();
    platform_panic("scheduler returned", 0);
}
#endif
