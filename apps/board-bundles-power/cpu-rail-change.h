/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BOARD_CPU_RAIL_CHANGE_H
#define SUNSTONE_BOARD_CPU_RAIL_CHANGE_H
/* Private CPU-step helper. The caller supplies the existing provider,
 * deadline, pause, register-read and failure functions. The host check runs
 * this same sequence against the actual provider and ownership registries. */

/* The vf0400 top points: DCDC3 for CPU-B, DCDC5 for CPU-L and the DSU. The
 * application refuses a bin whose table tops differ. */
#define CPU_RAIL_TOP_DCDC3_MV 1050u
#define CPU_RAIL_TOP_DCDC5_MV 1000u
/* The vendor regulator waits 1 ms for any change of up to 250 mV. */
#define CPU_RAIL_MAX_STEP_MV 250u

/* Where a rail change stands, for the failure line: the rail and the last
 * request submitted. A failure at CPU_RAIL_WRITE or later may have changed
 * the selector; the provider's bytes then say whether it did. */
enum { CPU_RAIL_IDLE, CPU_RAIL_READ, CPU_RAIL_WRITE, CPU_RAIL_SETTLE, CPU_RAIL_FINAL, CPU_RAIL_RELEASE };
static struct { unsigned rail, stage; } cpu_rail_state;

static void cpu_rail_pause(uint64_t end, const char *what)
{
    REQUIRE(bb_power_now() < end, what);
    bb_power_pause(what);
    REQUIRE(bb_power_now() < end, what);
}

/* Verify live clock controls while their exclusive clock lease is held.
 * DCDC5 supplies both CPU-L and the DSU; DCDC3 supplies CPU-B. */
static void cpu_rail_boot_clocks(enum a733_cpu_domain domain)
{
    static const uint32_t boot[A733_CPU_DOMAINS][2] = {
        {0xf8802600u, 0x03010000u}, {0xf8802700u, 0x03000000u}, {0xf8802600u, 0x03010105u}};
    for (unsigned d = 0; d < A733_CPU_DOMAINS; ++d) {
        if ((domain == A733_CPU_BIG) != (d == A733_CPU_BIG))
            continue;
        const uintptr_t base = 0x08871000u + 0x1000u * d;
        uint32_t pll, mux, pat0, pat1, lfm;
        REQUIRE(cpu_read(NULL, base, &pll) && cpu_read(NULL, base + 0x1c, &mux) &&
                cpu_read(NULL, base + 4, &pat0) && cpu_read(NULL, base + 8, &pat1) &&
                cpu_read(NULL, base + 0x18, &lfm), "CPU rail clock read");
        REQUIRE(pll == boot[d][0] && mux == boot[d][1] && !(pat0 & (3u << 29)) &&
                !(pat1 >> 31) && !(lfm >> 31), "CPU rail boot clocks");
    }
}

/* Each request names its own failures: read is the snapshot before the
 * write, write the selector write and its readback, final the snapshot after
 * settling. */
static void cpu_rail_request(const struct a733_rail_token *token, enum a733_cpu_domain domain,
                             enum a733_rail rail, uint64_t end, int write,
                             uint8_t expected, uint8_t target, struct a733_pmic_snapshot *out)
{
    const int final = cpu_rail_state.stage == CPU_RAIL_SETTLE;
#define CPU_RAIL_WHAT(text) (write ? "CPU rail write " text : final ? "CPU rail final " text : "CPU rail read " text)
    struct a733_pmic_request request = {0};
    enum a733_pmic_status result;
    cpu_rail_state.stage = write ? CPU_RAIL_WRITE : final ? CPU_RAIL_FINAL : CPU_RAIL_READ;
    do {
        REQUIRE(bb_power_now() < end, CPU_RAIL_WHAT("submit deadline"));
        /* A preceding snapshot can yield to the provider. Recheck the live
         * controls immediately before each attempted selector submission. */
        if (write)
            cpu_rail_boot_clocks(domain);
        result = write ? a733_pmic_submit_write(&bb_power.pmic, VOLT_OWNER, token, rail, expected, target,
                                                end, &request)
                       : a733_pmic_submit(&bb_power.pmic, VOLT_OWNER, token, rail, end, &request);
        if (result == A733_PMIC_BUSY)
            cpu_rail_pause(end, CPU_RAIL_WHAT("submit deadline"));
    } while (result == A733_PMIC_BUSY);
    REQUIRE(result == A733_PMIC_PENDING, CPU_RAIL_WHAT("submit"));
    while ((result = a733_pmic_result(&bb_power.pmic, &request, out)) == A733_PMIC_PENDING ||
           result == A733_PMIC_BUSY)
        cpu_rail_pause(end, CPU_RAIL_WHAT("result deadline"));
    REQUIRE(result == A733_PMIC_COMPLETE && out->rail == rail, CPU_RAIL_WHAT("result"));
    while ((result = a733_pmic_reap(&bb_power.pmic, &request)) == A733_PMIC_BUSY)
        cpu_rail_pause(end, CPU_RAIL_WHAT("reap deadline"));
    REQUIRE(result == A733_PMIC_COMPLETE, CPU_RAIL_WHAT("reap"));
#undef CPU_RAIL_WHAT
}

/* DCDC3 supports the existing 800/1050 mV steps. Either rail may also be
 * lowered to 800 mV from any 10 mV step up to the vf0400 top point on that
 * rail, with its clocks at their boot words. No change exceeds 250 mV, so
 * the 1 ms settle covers each.
 * One deadline bounds ownership, provider work, settling and release. */
static void cpu_rail_change(enum a733_cpu_domain domain, uint32_t from_mv, uint32_t mv,
                            struct a733_pmic_snapshot *out)
{
    const uint32_t top_mv = domain == A733_CPU_BIG ? CPU_RAIL_TOP_DCDC3_MV : CPU_RAIL_TOP_DCDC5_MV;
    REQUIRE((domain == A733_CPU_BIG || domain == A733_CPU_LITTLE) && from_mv != mv &&
            (from_mv > mv ? from_mv - mv : mv - from_mv) <= CPU_RAIL_MAX_STEP_MV &&
            ((domain == A733_CPU_BIG && (from_mv == 800 || from_mv == 1050) && (mv == 800 || mv == 1050)) ||
             (mv == 800 && from_mv > 800 && from_mv <= top_mv && from_mv % 10 == 0)), "CPU rail target");
    const enum a733_rail id = domain == A733_CPU_BIG ? A733_RAIL_DCDC3 : A733_RAIL_DCDC5;
    const a733_clock_mask_t clocks = domain == A733_CPU_BIG ? A733_CLOCK_CPU_B :
                                                           A733_CLOCK_CPU_L | A733_CLOCK_CPU_DSU;
    cpu_rail_state.rail = id;
    cpu_rail_state.stage = CPU_RAIL_READ;
    const uint64_t end = request_deadline();
    struct a733_clock_token hold = {0};
    struct a733_rail_token rail = {0};
    REQUIRE(a733_clock_acquire(&bb_power.clocks, VOLT_OWNER, 0, clocks, end, &hold) == A733_CLOCK_OK,
            "CPU rail clock lease");
    cpu_rail_boot_clocks(domain);
    enum a733_rail_result rr = a733_rail_acquire(&bb_power.rails, VOLT_OWNER, 0, A733_RAIL_BIT(id), end, &rail);
    REQUIRE(rr == A733_RAIL_PENDING || rr == A733_RAIL_READY, "CPU rail lease");
    while ((rr = a733_rail_poll(&bb_power.rails, &rail)) != A733_RAIL_READY) {
        REQUIRE(rr == A733_RAIL_PENDING || rr == A733_RAIL_BUSY, "CPU rail lease poll");
        cpu_rail_pause(end, "CPU rail lease deadline");
    }
    cpu_rail_request(&rail, domain, id, end, 0, 0, 0, out);
    REQUIRE(out->enabled && out->decoded && out->programmed_uv == from_mv * 1000u, "CPU rail before write");
    cpu_rail_request(&rail, domain, id, end, 1, (uint8_t)(out->selector & 127u),
                     (uint8_t)((mv - 500u) / 10u), out);
    REQUIRE(out->enabled && out->decoded && out->programmed_uv == mv * 1000u, "CPU rail readback");
    cpu_rail_state.stage = CPU_RAIL_SETTLE;
    uint64_t settled;
    /* One extra microsecond covers quantization of the counter clock. */
    REQUIRE(a733_deadline_after(bb_power_now(), VOLT_SETTLE_US + 1u, &settled) && settled < end,
            "CPU rail settle deadline");
    while (bb_power_now() < settled)
        cpu_rail_pause(end, "CPU rail settle deadline");
    cpu_rail_request(&rail, domain, id, end, 0, 0, 0, out);
    REQUIRE(out->enabled && out->decoded && out->programmed_uv == mv * 1000u, "CPU rail after settle");
    cpu_rail_state.stage = CPU_RAIL_RELEASE;
    while ((rr = a733_rail_release(&bb_power.rails, &rail, end)) != A733_RAIL_READY) {
        REQUIRE(rr == A733_RAIL_PENDING || rr == A733_RAIL_BUSY, "CPU rail release");
        cpu_rail_pause(end, "CPU rail release deadline");
    }
    enum a733_clock_result cr;
    while ((cr = a733_clock_release(&bb_power.clocks, &hold, end)) == A733_CLOCK_BUSY)
        cpu_rail_pause(end, "CPU rail clock release deadline");
    REQUIRE(cr == A733_CLOCK_OK, "CPU rail clock release");
    cpu_rail_state.stage = CPU_RAIL_IDLE;
}
#endif
