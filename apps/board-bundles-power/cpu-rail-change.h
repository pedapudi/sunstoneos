/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SUNSTONE_BOARD_CPU_RAIL_CHANGE_H
#define SUNSTONE_BOARD_CPU_RAIL_CHANGE_H
/* Private CPU-step helper. The caller supplies the existing provider,
 * deadline, pause, register-read and failure functions. The host check runs
 * this same sequence against the actual provider and ownership registries. */

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

static void cpu_rail_request(const struct a733_rail_token *token, enum a733_cpu_domain domain,
                             enum a733_rail rail, uint64_t end, int write,
                             uint8_t expected, uint8_t target, struct a733_pmic_snapshot *out)
{
    struct a733_pmic_request request = {0};
    enum a733_pmic_status result;
    do {
        REQUIRE(bb_power_now() < end, "CPU rail submit deadline");
        /* A preceding snapshot can yield to the provider. Recheck the live
         * controls immediately before each attempted selector submission. */
        if (write)
            cpu_rail_boot_clocks(domain);
        result = write ? a733_pmic_submit_write(&bb_power.pmic, VOLT_OWNER, token, rail, expected, target,
                                                end, &request)
                       : a733_pmic_submit(&bb_power.pmic, VOLT_OWNER, token, rail, end, &request);
        if (result == A733_PMIC_BUSY)
            cpu_rail_pause(end, "CPU rail submit deadline");
    } while (result == A733_PMIC_BUSY);
    REQUIRE(result == A733_PMIC_PENDING, "CPU rail submit");
    while ((result = a733_pmic_result(&bb_power.pmic, &request, out)) == A733_PMIC_PENDING ||
           result == A733_PMIC_BUSY)
        cpu_rail_pause(end, "CPU rail result deadline");
    REQUIRE(result == A733_PMIC_COMPLETE && out->rail == rail, "CPU rail result");
    while ((result = a733_pmic_reap(&bb_power.pmic, &request)) == A733_PMIC_BUSY)
        cpu_rail_pause(end, "CPU rail reap deadline");
    REQUIRE(result == A733_PMIC_COMPLETE, "CPU rail reap");
}

/* DCDC3 supports the existing 800/1050 mV steps. The only DCDC5 write here
 * restores 800 mV after the ROM has reset CPU-L and the DSU to boot rates.
 * One deadline bounds ownership, provider work, settling and release. */
static void cpu_rail_change(enum a733_cpu_domain domain, uint32_t from_mv, uint32_t mv,
                            struct a733_pmic_snapshot *out)
{
    REQUIRE((domain == A733_CPU_BIG && (from_mv == 800 || from_mv == 1050) && (mv == 800 || mv == 1050)) ||
            (domain == A733_CPU_LITTLE && from_mv == 1000 && mv == 800), "CPU rail target");
    const enum a733_rail id = domain == A733_CPU_BIG ? A733_RAIL_DCDC3 : A733_RAIL_DCDC5;
    const a733_clock_mask_t clocks = domain == A733_CPU_BIG ? A733_CLOCK_CPU_B :
                                                           A733_CLOCK_CPU_L | A733_CLOCK_CPU_DSU;
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
    uint64_t settled;
    /* One extra microsecond covers quantization of the counter clock. */
    REQUIRE(a733_deadline_after(bb_power_now(), VOLT_SETTLE_US + 1u, &settled) && settled < end,
            "CPU rail settle deadline");
    while (bb_power_now() < settled)
        cpu_rail_pause(end, "CPU rail settle deadline");
    cpu_rail_request(&rail, domain, id, end, 0, 0, 0, out);
    REQUIRE(out->enabled && out->decoded && out->programmed_uv == mv * 1000u, "CPU rail after settle");
    while ((rr = a733_rail_release(&bb_power.rails, &rail, end)) != A733_RAIL_READY) {
        REQUIRE(rr == A733_RAIL_PENDING || rr == A733_RAIL_BUSY, "CPU rail release");
        cpu_rail_pause(end, "CPU rail release deadline");
    }
    enum a733_clock_result cr;
    while ((cr = a733_clock_release(&bb_power.clocks, &hold, end)) == A733_CLOCK_BUSY)
        cpu_rail_pause(end, "CPU rail clock release deadline");
    REQUIRE(cr == A733_CLOCK_OK, "CPU rail clock release");
}
#endif
