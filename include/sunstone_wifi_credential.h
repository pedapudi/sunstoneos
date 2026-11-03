#ifndef SUNSTONE_WIFI_CREDENTIAL_H
#define SUNSTONE_WIFI_CREDENTIAL_H
#include <stddef.h>
#include <stdint.h>
/* The network the board was launched with. The host daemon stores the key for
 * the network named at submission in the session; the OS hands it to the
 * supplicant through this one call. Returns 1 and fills every output when a
 * credential is present, 0 when none was supplied.
 *   ssid, ssid_len: the network name, 1 to 32 bytes, not NUL-terminated.
 *   pmk32:          the 32-byte WPA2 pairwise master key (always present).
 *   password:       the WPA3 SAE password, or NULL when only the PMK is stored.
 *   password_len:   its length in bytes; 0 when password is NULL.
 * The pointers refer to read-only storage that lives as long as the session.
 * The caller never logs, copies into a result or retains the key beyond use. */
int ss_wifi_credential(const uint8_t **ssid, size_t *ssid_len, const uint8_t **pmk32,
                       const uint8_t **password, size_t *password_len);
#endif
