#include "a733_npu_application.h"
/* Link/interpretation fixture only. Established masks are zero, so the actual
 * image refuses execution. The instruction model changes its private RAM copy
 * after checking that refusal; it never creates a board-qualified artifact. */
const struct a733_npu_application_boot a733_npu_application_boot={
    .profile={.epoch=5,.generation=7,.lease=9,.npu_evidence=17,.command_admission=18,
        .usb_evidence=19,.pmic_evidence=20,.pin_evidence=21,
        .npu_owner=1,.pmic_owner=2,.usb_owner=3,.bridge_owner=4},
    .npu={.fabric={0x03000001,0x82000001,0x84000000},
        .rate_hz={300000000,400000000,400000000},.owner=1,.handoff=17},
    .pmic={.owner=2,.identity=20,.pins={{2,0,0},{2,0,0}}},
    .pins={.authorized={[A733_PIN_L]=3}}
};
