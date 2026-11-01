#ifndef SUNSTONE_USB_HID_H
#define SUNSTONE_USB_HID_H
#include <stddef.h>
#include <stdint.h>

#define SUNSTONE_HID_REPORTS 8
#define SUNSTONE_HID_FIELDS 48
#define SUNSTONE_HID_USAGES 32
#define SUNSTONE_HID_DESCRIPTOR_BYTES 1024
#define SUNSTONE_HID_REPORT_BYTES 64

/* ATTACH/DETACH usage bits describe decoded capabilities. */
#define SUNSTONE_HID_KEYBOARD 1u
#define SUNSTONE_HID_POINTER 2u
enum sunstone_hid_event_type {
    SUNSTONE_HID_ATTACH=1, SUNSTONE_HID_KEY, SUNSTONE_HID_BUTTON,
    SUNSTONE_HID_MOTION, SUNSTONE_HID_DETACH
};
struct sunstone_hid_event {
    uint64_t generation;
    uint8_t address, instance, type, report_id;
    uint16_t usage;
    int32_t value, x, y, wheel;
};
typedef void (*sunstone_hid_emit)(void *,const struct sunstone_hid_event *);
struct sunstone_hid_field {
    uint16_t bit, count;
    uint8_t size, report, flags, kind, usages;
    uint32_t usage_min, usage_max, usage[SUNSTONE_HID_USAGES];
    int64_t logical_min, logical_max;
};
struct sunstone_hid_report {
    uint16_t bits;
    uint8_t id, keys[32];
    uint32_t buttons;
};
/* Caller-owned, zero-initialized state; all operations run on one USB owner task.
 * emit copies the event before returning and must not reenter this interface.
 * The transport owns TinyUSB and DMA buffers; no pointer into a report survives.
 */
struct sunstone_usb_hid {
    uint64_t generation;
    sunstone_hid_emit emit;
    void *context;
    uint8_t address, instance, mounted, numbered, reports, fields, capabilities;
    struct sunstone_hid_report report[SUNSTONE_HID_REPORTS];
    struct sunstone_hid_field field[SUNSTONE_HID_FIELDS];
};
/* Return 1 on success, 0 for malformed/unsupported descriptor or stale identity.
 * Mount requires Report protocol, including for boot-capable interfaces.
 */
int sunstone_usb_hid_mount(struct sunstone_usb_hid *,uint8_t address,uint8_t instance,
    uint64_t generation,const uint8_t *descriptor,size_t bytes,sunstone_hid_emit,void *);
/* Return 1 for an accepted report, 0 for invalid/stale input, 2 for keyboard
 * rollover (state is preserved). Length must match the selected report exactly.
 */
int sunstone_usb_hid_report(struct sunstone_usb_hid *,uint64_t generation,
    const uint8_t *report,size_t bytes);
/* Emits held key/button releases before DETACH; stale generations do nothing. */
int sunstone_usb_hid_unmount(struct sunstone_usb_hid *,uint64_t generation);
#endif
