#ifndef SUNSTONE_BOARD_STOP_NOTE_H
#define SUNSTONE_BOARD_STOP_NOTE_H
/* The failure note that the storage-maintenance hook leaves for a stop (apps/board/main.c a733_storage_maintenance_stop). Needs board_fail_note, sunstone_crumb_el3_fail and the
 * site macros of sunstone_crumb.h. A status of zero is a clean exit and leaves nothing; any other status records the first failure if none is recorded, with the value 0xfc01 from
 * EL0 or EL1 (through the SVC) and 0xfc03 from EL3 (direct stores). */
static inline void a733_storage_maintenance_note(unsigned status,unsigned el,const void *caller)
{
    const uint32_t site=SUNSTONE_CRUMB_FAIL_SITE(BOARD)|SUNSTONE_CRUMB_FAIL_RETURN;
    if(!status)return;
    if(el==3)sunstone_crumb_el3_fail(site,0xfc03,caller);
    else board_fail_note(site,0xfc01,caller);
}
#endif
