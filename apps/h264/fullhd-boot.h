#ifndef SUNSTONE_H264_FULLHD_BOOT_H
#define SUNSTONE_H264_FULLHD_BOOT_H
#include "boot.h"
struct h264_fullhd_boot_contract {
    struct a733_smp_contract base;
    uint8_t session_digest[32],entry_digest[32];
};
_Static_assert(sizeof(struct h264_fullhd_boot_contract)==288,"full-HD private boot record");
/* Accessors accept only the retained primary EL3 monitor copy and compare the
 * complete live288-byte source. No temporary board-view pointer is published. */
const struct h264_fullhd_boot_contract *h264_fullhd_boot_record(const struct a733_smp_contract *);
int h264_fullhd_board_prepare(const struct a733_smp_contract *);
#endif
