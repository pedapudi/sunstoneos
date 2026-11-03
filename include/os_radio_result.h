#ifndef SUNSTONE_OS_RADIO_RESULT_H
#define SUNSTONE_OS_RADIO_RESULT_H
#include <stdint.h>
/* Where the host finds the radio service's result after the return. The record
 * (include/a733_radio_result.h) is as long as its scan list, so the service
 * copies it into memory sized to its length and publishes this fixed descriptor
 * in the image's BSS under the symbol os_radio_result_descriptor. The host reads
 * the descriptor, then the record at the address it gives, in full. */
#define OS_RADIO_RESULT_MAGIC UINT64_C(0x3144534152534f53) /* bytes "SOSRASD1" */
struct os_radio_result_descriptor {
    uint64_t magic;
    uint64_t address; /* Physical address of the encoded record; 8-byte aligned. */
    uint64_t bytes;   /* Its length, equal to the total in the record's own head. */
};
_Static_assert(sizeof(struct os_radio_result_descriptor)==24,"radio result descriptor");
#endif
