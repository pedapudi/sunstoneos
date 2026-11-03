/* The failure note of the storage-maintenance hook (apps/board/stop_note.h): a clean exit (status 0) leaves nothing, from EL1 or EL3; a stop with a status leaves the first-failure note,
 * through the SVC path at EL1 and the direct-store path at EL3, with the value that names the level. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#define SUNSTONE_CRUMB_FAIL_RETURN 0x80000000u
#define SUNSTONE_CRUMB_FAIL_SITE(group) (7u << 12)
static unsigned el1_notes, el3_notes; static uint32_t el1_value, el3_value;
static void board_fail_note(uint32_t site, uint32_t value, const void *caller) { (void)site; (void)caller; ++el1_notes; el1_value = value; }
static void sunstone_crumb_el3_fail(uint32_t site, uint32_t value, const void *caller) { (void)site; (void)caller; ++el3_notes; el3_value = value; }
#include "../apps/board/stop_note.h"
int main(void)
{
    a733_storage_maintenance_note(0, 1, 0); a733_storage_maintenance_note(0, 3, 0); a733_storage_maintenance_note(0, 0, 0);
    assert(!el1_notes && !el3_notes);                                   /* a clean exit through a733_smp_exit_privileged(0, ...) or the EL3 success SMC */
    a733_storage_maintenance_note(4, 1, 0);
    assert(el1_notes == 1 && !el3_notes && el1_value == 0xfc01u);       /* a privileged exit with a status */
    a733_storage_maintenance_note(6, 3, 0);
    assert(el1_notes == 1 && el3_notes == 1 && el3_value == 0xfc03u);   /* an EL3 stop */
    puts("PASS: a clean exit leaves no failure note; a non-zero status leaves one, by the SVC at EL1 and by direct stores at EL3");
    return 0;
}
