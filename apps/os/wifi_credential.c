/* The Wi-Fi credential receiver of the OS image (include/sunstone_wifi_credential.h, docs/wifi-credentials.md).
 * Stub: no credential is ever present. */
#include "sunstone_wifi_credential.h"

int ss_wifi_credential(const uint8_t **ssid, size_t *ssid_len, const uint8_t **pmk32, const uint8_t **password,
                       size_t *password_len)
{
    if (ssid) *ssid = 0;
    if (ssid_len) *ssid_len = 0;
    if (pmk32) *pmk32 = 0;
    if (password) *password = 0;
    if (password_len) *password_len = 0;
    return -2;   /* ENOENT */
}

void os_wifi_credential_wipe(void) {}
