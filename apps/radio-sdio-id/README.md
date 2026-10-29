# Returning radio SDIO identity diagnostic

This application connects the shared PMIC provider, native radio platform,
enumerated SDC1 card and existing AIC8800 bootstrap transport. It performs the
fixed write `0x40500058 = 0x40` and two address-correlated identity reads through
CMD53. It does not attach firmware or a network client.

The native constructor and application failure paths are software-tested.
Fitted-board supply topology, PM output-latch provenance and inherited PM2
reset/oscillator behavior remain unresolved. No supplied authority record or
physical result currently permits this application to run on a board.
The [reusable discovery design](RETURNING.md) records the source audit, bounded
initial scope and missing drain/restoration proof required for another test in
the same powered lifetime.

## The producer requires supplied board evidence

`build.py --authority RECORD --evidence OUTPUT --platform-build BUILD` consumes
a reviewed record and its evidence files. `BUILD` is a central CMake build from
the same checkout, with the existing `a733-power-probe` target and
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`. The returning-build helper compiles the
current provider sources and records compiler dependencies and object identities.

The record uses schema `a733-radio-sdio-id-v1`. Its `identity`, `port` and
`lifetime` strings identify one board and powered lifetime. `four_bit` is
integer zero for one bit at 400 kHz, or one for four bits at 25 MHz. Each group
below names an absolute evidence-file `path` and its full `sha256` digest.

Each evidence file is JSON. Its schema appends the group name to
`a733-radio-sdio-id-v1-`. It repeats the same `identity`, `port` and `lifetime`,
sets `reviewed` to true, and supplies the required assertions and values.
Hashes establish file identity. The supplied assertions and their reviewed
evidence establish the claimed board conditions; the producer cannot discover
wiring or authenticate an output latch from input DATA reads.

| Evidence group | Required assertions | Exact supplied values |
| --- | --- | --- |
| `topology` | `fitted_board_routes_verified`, `enabled_3v3_vbat_1v8_io` | `rails` maps `vbat` to `dcdc1` and each of `pg`, `pm`, `vddio` to the verified `dcdc8`, `bldo5` or `cldo1`; `routing_evidence` explains the fitted routing |
| `pins` | `complete_pm_latch_authority`, `other_pm_data_writers_excluded`, `pm2_reset_lpo_stable`, `pg0_pg5_pm0_pm1_exclusive` | `pm_latch` covers all six PM output latches; `latch_origin` is `verified-writer-history` or `verified-reset-contract`; `latch_evidence` identifies its provenance; `expected` and `desired` each contain eight `[function,pull,drive]` triples ordered PG0–PG5, PM0, PM1 |
| `startup` | `fabric_parent_gates_stable`, `sdc1_gate_off_reset_asserted`, `store_ahb_mbus_msi_lite1_ready`, `sd_monitor_autogating_disabled`, `device_mappings_security_verified`, `sdc1_irq_and_other_owners_excluded` | `fabric` and `fabric_hz` each contain AHB, NSI and MBUS values; four-bit operation also requires `peri0_400mhz_branch_verified` |
| `bootstrap` | `awake_bootstrap`, `no_outstanding_debug_or_stale_confirmations`, `write_40500058_40_authorized` | `pad` is the source-backed transport policy: 1 preserves the pad setting, 2 selects the source's temporary `0x80` setting |
| `recovery` | `whole_board_power_removal`, `permanent_retention`, `one_attempt`, `no_reuse` | The board/lifetime identity binds the reviewed terminal recovery action |

Every listed assertion must be true. Missing files, mismatched hashes, another
board or lifetime, incomplete latch values and unsupported clock/pin values
are refused before building an admitted native profile. No default rail map or
latch value exists. The producer preserves the exact supplied evidence beside
the image and binds the native authority identifier to the record's digest.

The result manifest always has `execution_admitted=false`. Building an image
does not authorize a board operation. The coordinator must bind that image and
its frozen evidence to the same current board/lifetime through the existing
board procedure. This application adds no launcher or queue entry.

## Missing authority produces only an explicit refusal image

For software checks, `--refusal-image` replaces `--authority`. It compiles a
zero-authority profile. At native entry the application prints `REFUSED radio
SDIO identity` and exits with status 7 before `bb_power_snapshot_init` creates
any provider or task. Its manifest labels it `refusal_image=true`.

`check.py --evidence OUTPUT --output CHECKS` uses that linked ELF and the actual
producer to construct a separate model ELF. Synthetic evidence files identify
`software-model-not-a-board`; they are not physical qualification records. The
check models provider, platform and transport effects while executing the native
application, input validation, mapping admission and result publication.

## One absolute deadline bounds the owned lifetime

The constructor augments the shared pin handoff with PG0–PG5, PM0 and PM1 and
the complete PM latch. The unchanged shared constructor adds PMIC PL0/PL1 to
the same manager. The native radio adapter uses that manager, the same clock
and rail registries, the shared register mutex and the actual client task.
It validates Device mappings before radio hardware admission.

The existing returning monitor supplies a two-second backstop. Provider startup,
platform setup and the identity exchange share one deadline 1.5 seconds after
monitor entry. Every downstream operation uses that deadline; no poll renews
it. The application yields one scheduler tick between polls. The fixed exchange
requires three CMD53 writes and three CMD53 confirmation reads; each successful
transfer is counted by the existing transport.

Requested four-bit operation refuses a one-bit fallback before borrowing the
card. Only the platform's actual card can enter transport setup. Transport
completion must include both decoded identity replies and at least six
successful CMD53 transfers. The application checks the absolute deadline again
after each transport poll, so a late completion cannot become success.

Success and failure preserve the complete platform, card, transport and buffers.
Software retention stops further work without resetting the controller,
changing PM2, disabling rails or releasing ownership. A return to ROM does not
permit reinitialization. The powered lifetime ends with the reviewed power
removal, including after a successful diagnostic.

## Capture the retained result before physical recovery

The application writes sixteen little-endian 64-bit words at `0x42000080`,
after the existing 128-byte boot result and before the log at `0x42000100`:

`magic, authority, outcome, polls, transport_error, host_error, host_status,
host_irq, host_bytes, wide, card_hz, transfers, chip, subrevision, elapsed_us,
retained`.

`magic` is `0x524144494f494431`. Outcome zero means the identity exchange met
the application's completion conditions. The record preserves controller
observations before software retention changes its state. Log lines also name
the failed stage and report elapsed time, poll count, selected width/rate,
successful CMD53 count and both identity words.

Twelve native worker cases and nine producer refusals pass in
`~/.cache/sunstoneos/evidence/radio-native-20261025/checks-layout/`. The native
build is in the adjacent `native-complete/` directory. One additional case in
`constructor-final/` runs the real shared-provider and radio constructors before the
scheduler starts. It models only RTOS primitives, platform entry and counter
reads, and verifies shared pin, rail, clock and provider bindings. These checks establish
software behavior. The existing provider, radio-platform and transport evidence
remains applicable to those unchanged units. Actual startup time, CMD53 data
integrity, firmware loading and RF operation remain unqualified.
