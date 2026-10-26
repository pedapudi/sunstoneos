/* SPDX-License-Identifier: Apache-2.0 */
#include "profile.h"
#include "power.h"
#include "platform.h"
#include "boot.h"
#include "a733_radio_platform_native.h"
#include "a733_mapping_check.h"
#include <string.h>

static struct a733_radio_platform_native native;
static struct a733_radio_platform radio;
static struct a733_radio_platform_config config;
static struct a733_radio_borrow borrow;
static struct aic8800_sdio_transport transport;
static uint64_t deadline, last_now, begun, polls;
static unsigned stage;
#define RADIO_RESULT ((volatile struct radio_id_result *)(RESULT_BASE + 128))
_Static_assert(sizeof(struct radio_id_result) == 128, "result fits before retained log");

/* Validate the immutable input before creating the shared provider or tasks.
 * Device observations cannot substitute for the producer's wiring/latch proof. */
int radio_id_profile_valid(void)
{
    const struct radio_id_profile *p = &radio_id_profile;
    a733_clock_mask_t scopes;
    struct a733_radio_platform_config c = p->platform;
    c.deadline_us = c.lifetime_deadline_us = 1;
    if (p->magic != RADIO_ID_MAGIC || !p->authority || p->four_bit > 1 || p->reserved ||
        c.owner != RADIO_ID_OWNER || c.handoff != p->authority ||
        c.topology.identity != p->authority || p->platform.deadline_us || p->platform.lifetime_deadline_us ||
        !a733_radio_platform_scopes(&c, &scopes) ||
        p->pm_latch.identity != p->authority || p->pm_latch.owner != RADIO_ID_OWNER ||
        p->pm_latch.known_mask != a733_pin_bank_mask(A733_PIN_M) ||
        (p->pm_latch.value & ~p->pm_latch.known_mask) ||
        p->bootstrap.record != p->authority || p->bootstrap.address != 0x40500058 ||
        p->bootstrap.value != 0x40 || p->bootstrap.awake != 1 ||
        p->bootstrap.no_outstanding_debug_operations != 1 ||
        p->transport.variant != AIC8800_TRANSPORT_D80_BOOTSTRAP ||
        (p->transport.pad != AIC8800_PAD_KEEP && p->transport.pad != AIC8800_PAD_WRITE_80) ||
        p->transport.ticks_per_ms != 1000 || !p->transport.command_timeout ||
        !p->transport.poll_interval || !c.card.command_timeout || !c.card.ready_poll_interval ||
        !c.card.supported_ocr || (c.card.supported_ocr & ~0x00300000u)) return 0;
    for (unsigned i = 0; i < 8; ++i) {
        unsigned bank = i < 6 ? A733_PIN_G : A733_PIN_M, pin = i < 6 ? i : i - 6;
        if ((c.expected[i].function != 0 && c.expected[i].function != 15) ||
            c.desired[i].function != (i < 6 ? 2 : i == 6 ? 0 : 1) ||
            !a733_pin_config_valid_banked(bank, pin, c.expected[i]) ||
            !a733_pin_config_valid_banked(bank, pin, c.desired[i])) return 0;
    }
    return 1;
}

static int admission(void *cookie, const struct a733_radio_platform_config *c,
                     const struct a733_clock_token *token)
{
    if (cookie != &radio || c != &radio.config || !token || !token->id ||
        token->owner != RADIO_ID_OWNER || memcmp(c, &config, sizeof(config))) return -1;
    static const uintptr_t pages[] = {0x02000000, 0x07025000, 0x02002000, 0x02003000,
                                      0x04021000, 0x03400000, 0x07090000};
    static const struct a733_mapping_rule device = {A733_MAPPING_READ_WRITE, 1,
        {{A733_MAIR_DEVICE_NGNRNE, A733_SH_ANY}}, 1, 1};
    for (unsigned i = 0; i < sizeof(pages) / sizeof(*pages); ++i)
        if (!a733_mapping_check(aarch64_device_translate, pages[i], pages[i], &device)) return -1;
    return 1;
}

static _Noreturn void terminal(unsigned outcome, const char *reason)
{
    /* Preserve the observed result before software retention stops the host.
     * Retention does not reset, power off, dispose or permit another attempt. */
    struct radio_id_result r = {RADIO_ID_MAGIC, radio_id_profile.authority, outcome, polls,
        transport.error, radio.host.result.error, radio.host.result.status,
        radio.host.result.interrupts, radio.host.result.transferred, radio.host.wide,
        radio.host.card_hz, transport.completed_transfers, transport.identity.chip_word,
        transport.identity.subrevision_word, last_now >= begun ? last_now - begun : 0, 0};
    if (radio.initialized) {
        (void)a733_radio_platform_retain(&radio);
        r.retained = radio.retained;
        if (!radio.fault || !radio.retained) r.outcome = 1;
    }
    *RADIO_RESULT = r;
    __asm__ volatile("dsb sy" ::: "memory");
    struct bb_line line;
    bb_line_start(&line, "RADIO SDIO result");
    bb_line_dec(&line, "outcome", r.outcome); bb_line_dec(&line, "stage", stage);
    bb_line_dec(&line, "polls", polls); bb_line_dec(&line, "elapsed_us", r.elapsed_us);
    bb_line_end(&line); bb_power_print(&line);
    bb_line_start(&line, "RADIO SDIO data");
    bb_line_dec(&line, "cmd53", r.transfers); bb_line_dec(&line, "wide", r.wide);
    bb_line_dec(&line, "hz", r.card_hz); bb_line_hex(&line, "chip", r.chip);
    bb_line_hex(&line, "subrevision", r.subrevision);
    bb_line_end(&line); bb_power_print(&line);
    platform_puts(reason); platform_puts("\n");
    platform_puts(r.outcome ? "FAIL radio SDIO identity; resources retained\n" :
                  "COMPLETE radio SDIO identity; resources retained, power removal required\n");
    platform_exit(r.outcome ? 1 : 0);
}

static int timely(void)
{
    uint64_t now = bb_power_now();
    if (now < last_now) return 0;
    last_now = now;
    return now < deadline;
}

void radio_id_client(void *unused)
{
    (void)unused;
    bb_power_wait_ready();
    begun = RESULT->start_counter / 24;
    last_now = bb_power_now();
    if (!timely()) terminal(1, "radio startup deadline");
    for (;;) {
        if (!timely()) terminal(1, "radio absolute deadline");
        ++polls;
        if (!stage) {
            enum a733_radio_platform_result r = a733_radio_platform_poll(&radio);
            if (r == A733_RADIO_RETAINED || r == A733_RADIO_INVALID)
                terminal(1, "radio platform failed");
            if (r == A733_RADIO_READY) {
                struct sdio_card *card = NULL;
                if (radio_id_profile.four_bit && (!radio.host.wide || radio.host.card_hz != 25000000))
                    terminal(1, "requested four-bit clock unavailable");
                if (!a733_radio_platform_borrow(&radio, RADIO_ID_OWNER, &borrow, &card) || card != &radio.card)
                    terminal(1, "radio card borrow failed");
                stage = 1;
            }
        } else {
            enum a733_radio_platform_result r = a733_radio_platform_live(&radio, &borrow, deadline);
            if (r != A733_RADIO_READY && r != A733_RADIO_BUSY) terminal(1, "radio lifetime failed");
            if (r == A733_RADIO_READY) {
                if (stage == 1) {
                    if (!aic8800_transport_init(&transport, &radio.card, &radio_id_profile.transport) ||
                        !aic8800_transport_setup(&transport, deadline)) terminal(1, "radio transport setup refused");
                    stage = 2;
                } else {
                    aic8800_transport_poll(&transport);
                    if (!timely()) terminal(1, "radio operation crossed deadline");
                    if (transport.state == AIC8800_TRANSPORT_FAULT) terminal(1, "radio transport failed");
                    if (stage == 2 && transport.state == AIC8800_TRANSPORT_READY) {
                        if (!aic8800_transport_read_identity(&transport, &radio_id_profile.bootstrap, deadline))
                            terminal(1, "radio identity admission refused");
                        stage = 3;
                    } else if (stage == 3 && transport.state == AIC8800_TRANSPORT_COMPLETE) {
                        if (!transport.have[0] || !transport.have[1] || transport.completed_transfers < 6)
                            terminal(1, "radio CMD53 exchange incomplete");
                        terminal(0, "address-correlated bootstrap and identity confirmations received");
                    }
                }
            }
        }
        /* This diagnostic has three short messages; retain the existing
         * scheduler and the provider's higher priority between polls. */
        vTaskDelay(1);
    }
}

void vApplicationStackOverflowHook(TaskHandle_t handle, char *name)
{ (void)handle; platform_panic(name, 0); }

void kernel_main(void)
{
    platform_init();
    if (!radio_id_profile_valid()) {
        platform_puts("REFUSED radio SDIO identity: missing board/lifetime authority; no provider initialized\n");
        platform_exit(7);
    }
    bb_power.handoff.authorized[A733_PIN_G] = 0x3f;
    bb_power.handoff.authorized[A733_PIN_M] = 3;
    bb_power.handoff.latch[1] = radio_id_profile.pm_latch;
    if (!bb_power_snapshot_init(radio_id_client, "radio-id")) platform_panic("radio shared constructor", 0);
    config = radio_id_profile.platform;
    deadline = bb_power.deadline;
    config.deadline_us = config.lifetime_deadline_us = deadline;
    if (!a733_radio_platform_native_init(&native, &bb_power.locks, bb_power.client, admission, &radio) ||
        !a733_radio_platform_init(&radio, &config, &native.operations, &native,
            &bb_power.clocks, &bb_power.pins, &bb_power.rails, &bb_power.pmic) ||
        (radio_id_profile.four_bit && !a733_radio_platform_four_bit(&radio)))
        platform_panic("radio constructor refused; resources retained", 0);
    vTaskStartScheduler();
    platform_panic("radio scheduler returned", 0);
}
