# Design: NPU cold boot, recovery and power management

Status: **implemented** (startup, local stop, restart, retune, admission;
host-tested, with the finite startup and inference qualified on the board).
**Open**: the service-worker changes (interrupt-driven completion in the
multicore image, deadlines and cancel, client waits, per-job work removal,
idle gating of the local clocks, model loading from the card).

Section numbers refer to
[docs/performance-architecture.md](../performance-architecture.md). The design
covers NPU startup ([a733_npu_startup.c](../../drivers/npu/a733_npu_startup.c)),
the local clock tables and fabric rules
([a733_npu_platform.c](../../drivers/npu/a733_npu_platform.c)), the engine
controller ([a733_npu_controller.c](../../drivers/npu/a733_npu_controller.c))
and the service worker ([npu_service.c](../../drivers/npu/npu_service.c)).
Register-level detail is in [npu-startup.md](../npu-startup.md) and
[npu-pll.md](../npu-pll.md). The text, tables and figures that derived this
design, and the unrun budgets, are in
[the evidence records](../evidence/npu-platform.jsonl).

## Admission copies the session page once and validates the copy

The host launcher places a session page at `0x41f00000`; the EL3 contract
carries its SHA-256. The EL3 contract step (`a733_smp_contract_valid`) checks
only contract fields (magic, version, build ID, epoch, generation, lease,
window, deadlines, watchdog and redirect flags) and records the page digest
and supervisor identity in the retained receipt. It reads nothing from the
live page. The copy step (`npu_single_core_seal` at EL3, `npu_session_copy` at
EL1 in the multicore build) copies the page into retained private memory,
hashes the copy once and compares the hash with the receipt. A second compare
requires the copy's image digest, epoch, generation, lease and supervisor to
match the contract. Startup reads only the copy, so a writer that changes the
live page after the copy cannot affect startup.

The page carries identities and evidence digests: magic, version and size; the
image digest; epoch, generation and lease; the whole-platform reservation; four
reviewed-evidence digests (admission, clock, power, supervision); the
supervisor identity; the owner identities that have a consumer; and one fabric
copy that startup reads and the USB platform configuration also reads.

**Supervision is required on every session.** The contract requires the
watchdog off, so the only recovery is supervised whole-board power removal. The
host preparer (`apps/npu-convolution/prepare-supervised.py`) requires the
conditions `manual-whole-board-power-removal`,
`retain-all-resources-until-power-removal` and `stop-after-first-failure`, and
the clock condition `supervised-cross-pll-disturbance-risk`, because startup
always enables or adopts PLL_NPU and the reference manual warns that enabling a
PLL can disturb other PLLs.

## Startup is the only sequencer

`a733_npu_startup_step` runs one stage machine on the bootstrap task and
returns PENDING only when it waits on hardware, a provider or a deadline. A
call runs stages until one waits.

1. **Tokens.** Acquire the exclusive PLL_NPU and NPU-local clock scopes with
   the shared fabric ancestry, the DCDC2 rail token and one GPU/PCK resource
   lease covering the NPU master gates and the fabric parent.
2. **Supply.** Wait for the power authority to report DCDC2 verified. Startup
   makes no PMIC transfer of its own.
3. **Register pass.** Under one register-lock acquisition, with no write,
   check: the 24 MHz reference; the shared SRAM routed to the CPU; the AHB, NSI
   and MBUS words against the record, with no DFS update pending on the last two. It also checks: PERI0 when a fabric clock selects it; the video encode and decode
   gates closed and their bus word's gates closed and resets asserted. It then checks: the NPU module and bus gates closed, core, AXI and AHB resets asserted and SRAM
   reset released. It then checks: DRAM0 gated on from DDRPLL with the DRAM bus clocked by the
   PHY PLL. Finally, the pass checks: the autonomous clock modes off; PLL_NPU either disabled (with only
   the inherited reserved bit set outside documented fields) or already locked
   with its output open at the selected or the other supported rate; and the
   four PLL_NPU tuning words at reset. Any mismatch is a refusal before the
   first write.
4. **Domain.** Request domain 4 from the power-domain owner. An OFF domain is
   powered first; an ON domain is adopted without a PCK600 write and PLL_NPU
   starts while the owner answers.
5. **PLL_NPU.** Apply the method-2 enable table with readback, or read the
   adopted word, or retune an adopted PLL of the other rate, then wait for LOCK
   without the register lock: three observations 3 µs apart, then 20 µs apart,
   at most 100. A spin without LOCK retains with the output gate closed.
6. **Open.** Under the lock: open the PLL output, route the shared SRAM to the
   NPU, release the AHB, AXI and core resets, select PLL_NPU at divider 1, open
   the bus and module gates, each with readback. Then wait the 2 ms settle,
   blocking to the last tick and spinning the remainder on the raw counter.
7. **Final check** of the opened state, then READY.

The local clock open and stop tables, the PLL enable and retune tables and the
fabric parent table are `const` step tables applied by one
apply-with-readback helper; they follow the pinned Allwinner and VIPLite
sequences cited in the source. Fields outside a step's mask keep their read
value. Ownership is checked once per call (owning task, scheduler running, EL1
privilege), and a deadline is checked at call entry, after each lock release
and between steps.

**Operating point.** The default is the vendor point for the board's speed
bin, read from SID word `0x0300624c`: 852 MHz at 800 mV for bin codes `0x04`,
`0x05` and `0x12` (a 1,704 MHz oscillator, multiplier 71, P0 = 2, divider 1). Every other code is refused, because points above 800 mV need DCDC2
changes and DCDC2 also supplies eDP and HDMI. The 492 MHz point (multiplier 82, P0 = 4) is selectable before the scheduler starts with `a733_npu_startup_rate`.

**Local stop** closes the module and bus gates and asserts the three resets in
one call and one locked pass, routes the shared SRAM back to the CPU, and
leaves domain 4 on and PLL_NPU locked with its output open. **Restart** and
**retune** accept only that stopped state: restart re-runs the PLL and open
stages through the adoption path. `a733_npu_startup_retune` changes the
rate with the vendor order (output gate off, factors, lock enable cleared and
set, LOCK, output gate on) only while the NPU is stopped and the speed bin
allows it. A failed readback retains the clock, rail and lease tokens. Power-off
and power-restart requests are refused, because the shared SRAM and the DCDC2
supply have other users.

**Failure classes.** An ordinary refusal before the first write releases the
tokens. A fault after any write retains them, keeps the register lock if a CCU
sequence stopped midway, closes the NPU register window and records the first
failure in the crumb record.

## Interfaces and invariants

- `a733_npu_startup_init` runs before the scheduler, validates the
  configuration and providers, and builds the domain-4 row. `bind_task` binds
  the bootstrap task and the row's wake notification.
- `a733_npu_startup_prepared` returns 1 once READY with no failed counter
  clock, no failed clock registry and no retained register lock.
- The engine controller, local clock fields and startup state live in kernel
  `.bss` (Normal Write-Back, Inner Shareable), written only by the owning task
  and read by no DMA master. The worker owns the engine after activation and
  the bootstrap task blocks.
- The service keeps the only copy of the activation metadata (operations,
  backend cookie, MMU image, command table), fixed before the scheduler starts
  and checked once at `npu_service_init_deferred`.
- Domain 4 is never powered off: the shared SRAM aliases (`0x74000` to
  `0xf3fff`) are the NPU's internal SRAM and the ROM contract needs them.

## Qualified figures

| Figure | Value | Evidence |
| --- | --- | --- |
| Session-page hashes at admission | 1 | `npu-platform-host-model-comparison` in [npu-platform.jsonl](../evidence/npu-platform.jsonl) |
| Local stop | 1 call, 10 reads, 5 writes, 4 clock samples, 1 lock (register model) | same record |
| Startup with separate platform and PLL controllers (host model, before the single sequencer) | 3 and 6 polls, 51 and 43 reads, 9 or 10 two-core calls over 13 or 14 ticks | same record |
| Finite startup and inference on the board | startup 4.0 ms; inference 2.91 ms doorbell to interrupt at 852 MHz and 800 mV | [status/npu.md](../status/npu.md) |

The single sequencer in `a733_npu_startup.c` has not been re-counted in the
register models. Physical behavior of the cold master-gate path, the adoption
of an inherited ON domain and repeated transitions is unqualified.

## Limitations

- The DCDC2 settle (5,160 µs) accounted for about six two-core fixture calls
  in the model; startup waits only for the power authority's verified
  DCDC2 word, and the register-model counts of the single sequencer remain to be
  taken (open work 9).
- About 20.7 MB are hashed before activation (11 startup calls of 1,881,936
  bytes) because `npu_convolution.c` and `npu_session.c` hash on every startup
  poll; the target is one hash at the first bootstrap publication.
- A second session after a return needs whole-board power removal, because the
  return receipt cannot show which SRAM the next FEL load uses.
- Excluded on purpose: domain-4 power-off (the shared SRAM loses its contents),
  DCDC2 writes and points above 800 mV (the rail also supplies the display),
  disabling PLL_NPU at idle (each wake adds a lock wait and another cross-PLL
  enable), load-driven frequency changes (only the thermal level needs a lower
  point), and a CCU reset of an engine that is not idle (no evidence shows its
  bus transactions have drained, so that case retains).

## Open work

The figures, per-job tables and the nine board checks for each item are in the
evidence records `npu-platform-budget-targets`,
`npu-platform-per-job-work-and-digest`, `npu-platform-worker-gating-recovery`
and `npu-platform-board-evidence-plan`.

1. **Interrupt-driven completion in the multicore image.** The multicore
   worker polls once per 1 ms tick while the engine is active
   (1,000 wakes per second) because the image defines no `a733_irq_routes`
   table; SPI 65 stays disabled. Single-core profiles route SPI 65 (GIC
   interrupt 97) to CPU0 and bind it in `npu_service.c`. The step adds an
   `a733_irq_routes` table in `apps/npu-convolution` that routes SPI 65 to core
   1, as `apps/os/irq.c` does. No reserved code changes; the multicore image
   never returns, so it needs no GIC restore.
2. **Completion fallback and deadlines.** With the interrupt bound, the worker
   waits on its notification for at most 10 ms or the job deadline, then makes
   one `a733_npu_poll` pass: DONE with full idle (`0x7fffffff`) finishes and
   counts a missed interrupt; DONE without full idle stays latched. Replace
   `a733_npu_check_timeout` and the elapsed test in `a733_npu_advance` with
   three worker deadlines: job (1 s for the convolution application),
   bootstrap (10 ms for the 208-byte stream) and cancel. The worker still
   wakes every 1000 ms when idle.
3. **Cancel and restart.** On a missed deadline the worker masks interrupt 97,
   polls once, then calls `a733_npu_cancel`: a 100 µs assertion, then up to 1 ms
   for the acknowledgement (`0x3a8` bit 4) and full idle. After that, one poll per tick
   up to 20 ms (the vendor 200 × 100 µs bound). On success the job reports
   CANCELED, then `a733_npu_reap`, `a733_npu_close` and `a733_npu_open` with the
   soft reset and bootstrap stream run, and interrupt 97 is unmasked. Four
   recoveries per session, counting bootstrap and job deadlines, bound hangs;
   the fifth retains.
4. **Client waits.** The convolution client blocks on the bootstrap
   notification, holds `NPU_SERVICE_WAIT` after OPEN and SUBMIT, sends no
   STATUS request, and reads the output in place; REAP folds into the next
   SUBMIT or CLOSE. A job then costs two round trips.
5. **Per-job work.** Remove the per-job constant-buffer digest (917,504 bytes),
   `AT` translations, MMU table scan and output copy after the primary agent
   maps the admitted regions and the EL1 translation-table pages read-only with no writable alias. Use SHA2 instructions for the remaining digests where
   `ID_AA64ISAR0_EL1` reports them. Move cache maintenance to the writers: the
   client cleans its input (`DC CVAC`, then `DSB SY` before SUBMIT), the worker
   clean-invalidates the 4,096 output bytes before a job and invalidates them
   after the full-idle read. Remove the pool clear once a board run shows
   identical outputs from a pool prefilled with zeros and with `0xa5`.
6. **Idle gating of the local clocks.** After 100 ms without a job the worker
   closes the controller and masks interrupt 97, then gates the module and bus
   clocks and asserts the AHB, AXI and core resets in one locked pass. Domain 4, the PLL and the rail stay on. The next submit opens the gates and the
   controller (soft reset, 208-byte bootstrap stream) with submits queued
   during the wake. Gating is off until the read-only mappings exist and a
   board current measurement shows a saving. This design is separate from the
   internal clock gating in [npu_idle.c](../../drivers/npu/npu_idle.c), which
   gates the engine's own clock and is measured in
   [status/npu.md](../status/npu.md).
7. **Page tables.** `a733_npu_mmu_build` can use 1 MiB or 64 KiB pages per
   16 MiB root entry; `a733_npu_memory.c` requests 4 KiB for every buffer.
   Larger pages shrink the table the per-job scan reads and widen NPU TLB reach.
8. **Model loading from the card.** The first model larger than an image's
   linked budget chooses one load path: a cache-line-aligned slot shared with no
   other owner, unmapped from the NPU during the write and handed to the session
   by a release store; cache maintenance by address over the bytes written; and
   a SHA-256 over the slot after the last write.
9. **Counts for the single sequencer.** The register-model budget
   harnesses of the earlier controllers are no longer in the tree. Add a
   counting harness for `a733_npu_startup.c` and compare with the targets (at most 60 register reads outside LOCK
   samples, 12 writes, 16 clock samples plus one per LOCK sample, 3 lock
   acquisitions, 4 bootstrap ticks without PENDING).
10. **Board evidence**, in one session each because a second session needs
    power removal: the register-pass words against the constants; the SID
    speed bin and `ID_AA64ISAR0_EL1` per core type; startup to READY and
    conformance outputs at 492 and 852 MHz; one interrupt per job over a
    gate-and-wake run; a cancelled and recovered job followed by a correct
    inference; the pool-prefill and reset-release equivalence runs; board
    current with the NPU gated and ungated; and one hundred gate-and-wake
    cycles with unchanged outputs.
