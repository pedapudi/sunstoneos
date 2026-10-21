/* Control-request admission of the RAM diagnostics reset request.
 *
 * Reuses the protocol model, which compiles the actual
 * drivers/usb/diagnostic.c, and calls tud_vendor_control_xfer_cb directly.
 * Only the documented vendor, device-recipient OUT request 0x40/1 with zero
 * value, index and length resets the channel; every other request type must
 * stall without resetting it. checks/audit-device-minors-usb-control.py runs
 * it under Unicorn. */
#include "usb-diagnostic-protocol/model.c"

struct control_report { uint32_t done, failed, accepted, resets; } control_report;

void control_run(void)
{
    static const uint8_t refused[] = {
        0x00, /* standard, device */
        0x20, /* class, device: TinyUSB forwards it to the vendor driver of interface 0 */
        0x21, /* class, interface */
        0x41, /* vendor, interface */
        0x42, /* vendor, endpoint */
        0x60, /* reserved type, device */
        0xc0, /* vendor, device, IN */
    };
    for (unsigned i = 0; i < sizeof(refused); ++i) {
        tusb_control_request_t request = reset_setup;
        request.bmRequestType = refused[i];
        if (tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &request)) control_report.failed = 1 + i;
    }
    if (model_report.resets) control_report.failed = 100;
    control_report.accepted = tud_vendor_control_xfer_cb(0, CONTROL_STAGE_SETUP, &reset_setup);
    control_report.resets = model_report.resets;
    control_report.done = 1;
    finish();
}
