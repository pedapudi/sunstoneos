/* SPDX-License-Identifier: MIT
 * Register observations follow rgxmulticore.c and rgx_cr_defs_km.h.
 * Copyright (c) Imagination Technologies Ltd. All Rights Reserved.
 * See drivers/gpu/LICENSE.pvr.
 */
#include "pvr_compute_mode.h"
#include "pvr_firmware.h"
static int fail(struct pvr_compute_mode *m, unsigned error)
{
    if (m->phase != PVR_COMPUTE_MODE_FAILED) m->error = error;
    m->phase = PVR_COMPUTE_MODE_FAILED;
    return 0;
}
int pvr_compute_mode_count(struct pvr_compute_mode *m, uint64_t identity,
    uint64_t token, uint64_t epoch, uint32_t count)
{
    if (!m) return 0;
    if (m->phase != PVR_COMPUTE_MODE_NEW) return fail(m, PVR_COMPUTE_MODE_ORDER);
    m->identity = identity; m->token = token; m->epoch = epoch; m->cores = count;
    if (identity != PVR_FW_A733_BVNC) return fail(m, PVR_COMPUTE_MODE_IDENTITY);
    if (!token || !epoch) return fail(m, PVR_COMPUTE_MODE_LEASE);
    if (!count || count > 15) return fail(m, PVR_COMPUTE_MODE_COUNT);
    if (count != 1) return fail(m, PVR_COMPUTE_MODE_MULTICORE_REQUIRED);
    m->phase = PVR_COMPUTE_MODE_COUNTED;
    return 1;
}
int pvr_compute_mode_capabilities(struct pvr_compute_mode *m, uint64_t capabilities)
{
    if (!m) return 0;
    if (m->phase != PVR_COMPUTE_MODE_COUNTED) return fail(m, PVR_COMPUTE_MODE_ORDER);
    m->capabilities = capabilities;
    if ((capabilities & 0x18u) != 0x18u) return fail(m, PVR_COMPUTE_MODE_CAPABILITY);
    m->phase = PVR_COMPUTE_MODE_ADMITTED;
    return 1;
}
int pvr_compute_mode_valid(const struct pvr_compute_mode *m, uint64_t identity,
    uint64_t token, uint64_t epoch)
{
    return m && m->phase == PVR_COMPUTE_MODE_ADMITTED && !m->error &&
        identity == PVR_FW_A733_BVNC && m->identity == identity && token && epoch &&
        m->token == token && m->epoch == epoch && m->cores == 1 &&
        (m->capabilities & 0x18u) == 0x18u;
}
