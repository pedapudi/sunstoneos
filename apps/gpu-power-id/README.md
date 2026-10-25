# GPU power and identity diagnostic

This returning single-core diagnostic powers GPU_TOP and GPU_CORE on the
default GPU power path ([gpu-vendor-default.md](../../docs/designs/gpu-vendor-default.md)),
reads the GPU hardware identity, powers CORE and TOP off, does that twice, and
parks the shared PMIC service. The ON sequence read the identity
`36.56.104.183` on port 3-1 on 2026-10-25 from an image that made no OFF. The
OFF and a second ON in one lifetime are checked in software only.

The diagnostic creates no firmware, command buffers, GPU mappings or DMA
backing. Its successful return establishes a narrower boundary than firmware
startup or arithmetic execution. It cannot locate the instruction that stopped
an earlier full GPU image from returning.

## The never-booted identity path has an upstream unwind

The reference is the board Linux revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, already pinned by the GPU sources.
Its `imagination/pvr_device.c` powers the device before reading control registers
(lines 680–739). GPU initialization reads identity before requesting firmware
(lines 595–603). Failure there unwinds through synchronous runtime suspend.
`imagination/pvr_power.c` disables firmware during suspend only when firmware
has booted (lines 353–376).

That order supports this diagnostic's OFF transition when the admitted initial
state has never executed GPU firmware or commands. Closed local gates and an
asserted external reset are required, including when GPU_TOP is inherited ON.
Those register bits alone do not establish absence of prior execution or DMA.
The coordinator must establish that history and exclude every other writer
through return. This path does not provide recovery from a firmware hang.

The reference files are preserved with the candidate. Their SHA-256 identities
are `e81e5b2126db83517fc7435a96e0fddb48e6364b255747f300f448d10255805d`
for `pvr_device.c` and
`5ab4ff9c54cd8060f0feb6b5c8320da5000c1c24a09f30fbf8c03ef283b732d5`
for `pvr_power.c`. The identity decoding retains the Imagination notice in
[main.c](main.c); its license is [LICENSE.pvr](../../drivers/gpu/LICENSE.pvr).

## Existing owners perform power transitions

The initializer creates the existing PMIC service without a display-domain row.
The GPU platform registers its sole domain-5 row before scheduler startup.
Before the first ON the platform raises DCDC4 from 800 mV to the GPU operating
point's 960 mV under an exclusive DCDC4 lease, with a readback, and waits
1.2 ms; its supply consumer then samples DCDC4 and keeps a shared lease. The
GPU resource manager checks the inherited PERI0 400 MHz parent and DCDC4 at
960 mV before local clock changes.

The [resource manager](../../docs/gpu-resources.md) may enable the shared PPU,
AHB and MBUS gates. The [power owner](../../docs/power-domains.md) adopts coherent
GPU_TOP policy/status 0 or 8, and requests ON when a transition is needed.
Under the exclusive LOCAL lease, the GPU controller then captures CORE6
policy/status/PWCR. It requires whole policy/status words to match at 0 or 8,
or the power-up state of policy 8, status 0 and PWCR `0x101`. It writes PWCR
zero, enables PLL_GPU0, releases the GPU reset, opens the bus and core clock
gates, sets PLL_GPU0 to 1008 MHz, writes CORE6 policy 8 and polls CORE6
status until 8 before identity access. Every PLL_GPU0 write is read back.

Only these identity reads reach the GPU register window, in each cycle:

| Address | Width | Meaning |
| --- | --- | --- |
| `0x01800024` | 32 bits | High word of `RGX_CR_CORE_ID__PBVNC`: branch and version |
| `0x01800020` | 32 bits | Low word: units and configuration |

The vendor's probe reads the identity the same way (`rgxbvnc.c:119`;
`OSReadHWReg64` is two 32-bit loads, high word first, `osfunc.h:1086–1095`).
Each read requires an identity Device-nGnRnE mapping, held platform ownership,
and monotonic deadline checks before and after access. There are no GPU
register writes. The expected identity is `36.56.104.183`; another value ends
the run, after OFF and provider park, with `FAIL GPU identity not
36.56.104.183`.

OFF first writes CORE policy zero and polls status zero with TOP and the GPU
clocks still running. It then closes the bus gate and the core clock gate,
disables PLL_GPU0, asserts the GPU reset, requests domain-5 OFF, and releases
the platform's resource token. Resource release independently checks CORE
policy and status zero and PLL_GPU0 disabled. Completion requires
the actual OFF phases, no token or delegated lease, an answered OFF row with no
count, a healthy OFF domain with no users, and no retained client resource slot.
After service park, the client checks those conditions again and refuses any
remaining PMIC request or borrow. The DCDC4 stable rail lease remains held until
the image lifetime ends, and DCDC4 stays at 960 mV. Shared gates remain enabled
for their other users.

The client uses the existing absolute 1.5-second service deadline from monitor
entry. Each PCK transition also has the owner's 10-millisecond bound. Pending
client polls wait one scheduler tick; elapsed bounds are observed on resumption.
The returning CPU monitor retains its two-second deadline and makes the vendor
BL31's cold-boot security writes before EL1 starts. A failure can return a CPU
error report while partial device ownership remains. Every failed or missing
completion retains the claim and owned RAM for coordinator recovery; ROM
visibility alone does not authorize reuse.

## The power-only variant runs before any identity probe

`GPU_POWER_ONLY` builds a variant of the same client that runs the same two
cycles without the identity reads. Each cycle powers TOP and CORE, records
the controller's final CORE status read, which ON completion requires to be 8,
then powers CORE and TOP OFF and releases the platform token. After the second
cycle the client parks the provider. The second ON shows that OFF left a state
from which ON succeeds. The variant makes no access to the GPU register window at
`0x01800000`: the identity reads and their mapping check are compiled out.
Its completion text is
`COMPLETE GPU power only: CORE status 8, OFF and provider parked`, after the
lines `GPU power cycle=1 core_status=0x8` and `GPU power cycle=2
core_status=0x8`.

Both variants print the controller's cached CORE6 observations after each
ON and on any failure, without another device read. Each line carries the
cycle number, starting at 1:

- `GPU CORE entry`: the seen mask (255 when every observation completed)
  and the policy, status and PWCR words before any write.
- `GPU CORE inputs`: `PPU_IDR0`, and `PPU_MISR` before any write and before
  the final status poll.
- `GPU CORE after`: the status after the PWCR write and after the policy
  write.
- `GPU clock`: the last PLL_GPU0 word read, the reads of each PLL lock wait
  and the reads until each GPU clock update bit cleared.

Both variants also print a read-only snapshot of the candidate CORE6
prerequisites, one `GPU SNAP n=<k> a=<address> v=<value>` line per word.
Snapshot 0 precedes the first ON, and snapshot n follows the ON attempt of
cycle n, including a failed or expired one. The 35 words are the CORE6 and
TOP5 PPU rows, R_CCU `0x1ac` and `0x250`, and CCU `0x0e0`, `0xb20` and
`0xb24`; none is in the GPU register window. Each page is checked as an
identity Device-nGnRnE mapping before its reads.

A failure also prints a `GPU resources` line: the platform's last resource
result and the resource manager's error, stage and last value. A release
that finds CORE6 policy or status nonzero after TOP5 OFF fails there with
error `A733_GPU_RESOURCE_LIVE`.

The power-only variant tests OFF and the second ON without a GPU register
read, so it has the lower recovery risk. On a recovered board it runs after
the frozen provider-readiness checkpoint and before the identity variant,
which runs only after the power-only variant has returned on that board.
Admission is the same for both variants.

## The wrapper-read variant reads four GPU wrapper words

`wrapper.c` builds the wrapper-read variant. It compiles `main.c` as the
power-only variant with that variant's client and entry renamed, so the
power-only source and binary stay unchanged, and adds its own client and
entry. Its client:

1. takes snapshot 0 and runs one ON of the default sequence, waiting at
   most 200 ms for CORE6 status 8; at least 100 ms of the client deadline
   must remain after that bound;
2. takes snapshot 1 and prints the controller's phase, error, step and last
   read, the resource manager's record and the CORE6 lines, without ending
   the run;
3. refuses the wrapper reads unless the controller completed the GPU clock
   steps, shown by its MISR read before the final status poll;
4. reads `0x01880104` (`pwr_qchan_cfg`), `0x01880100` (`pwr_cmp_dly`),
   `0x01880018` (`mult_core_cnt`) and `0x0188001c` (`mult_core_id`) in that
   order, each a 32-bit load after an identity mapping check of the page and
   a deadline check. Before each load it writes the read number and a
   counter stamp into the progress record at result offset 128 (magic
   `GPUWRP01`, stage, completed reads, stamp) and prints `GPU WRAPPER
   reading n=<k> a=<address>`; after it, `GPU WRAPPER n=<k> a=<address>
   v=<value>`;
5. prints the decoded `GPU WRAPPER qchan` and `GPU WRAPPER delay` lines;
6. with CORE6 not ON, ends with `FAIL GPU wrapper read: CORE6 not ON, four
   wrapper words read` (status 1); with CORE6 ON, powers OFF, parks the
   provider and prints `COMPLETE GPU wrapper read: CORE status 8, four
   wrapper words, OFF and provider parked`.

The four addresses are the `wrapper_words` table; the variant makes no
other GPU-window access and no GPU-window write. A read that never
completes stops the CPU: the run then does not return, and only a power
removal recovers the board.

`check.py --wrapper-read` runs seven cases on the linked image: the board's
state with the platform deadline and with the client's own bound, CORE6 ON
with OFF and park, a stop before the clock and reset steps, a wrong
wrapper-page translation, an OFF failure after the reads, and a seeded
mutant whose second table entry names another word, which the check
refuses at that read. In every case the only GPU-window accesses are 32-bit
loads of a prefix of the four words, in order, each preceded by its
progress record and its log line, with both snapshots and the CORE6 lines
printed before the first. A store to the window fails the check. Bundle
preparation (`prepare.py`) requires the image's `wrapper_words` table to
hold exactly the four words in order and every host-check case to read only
a prefix of them.

## The probe-read variant reads the core identity as the vendor's probe does

`probe-read.c` builds the probe-read variant the way `wrapper.c` builds the
wrapper-read variant. After the default ON sequence, which must reach CORE6
status 8 within 200 ms, it
reads `drm_ctrl` (`0x01880014`), `irq_ctrl` (`0x01880010`) and the SMC0
word `0x0a0000b0`, then the core identity as the vendor's probe reads it:
two 32-bit reads, `0x01800024` and then `0x01800020`. It then writes 2 to
`0x0a0000b0` and 0 to `0x01880014`, as the vendor `sunxi_secure_config`
does, each read back, reads the identity again, and writes both words back
to their entry values, each read back. From the first write to the last
restore readback it masks D, A, I and F, makes no failure call, and points
`VBAR_EL1` at restore vectors that write both words back before the
image's own exception handler runs; it starts that window only while at
least 500 ms remain before the monitor's 2 s deadline, and otherwise
returns status 7 with nothing written. After the restore it makes the OFF,
the supply end and the provider park of the power-only variant. The fifteen accesses are the rows
of `probe_steps`; the progress record at result offset 128 (magic
`GPUPRD01`) holds the row in flight and every completed value. The
completion text is `COMPLETE GPU probe read: identity 36.56.104.183 before
and after the secure configuration, restored, OFF and provider parked`.
[gpu-power.md](../../docs/gpu-power.md#the-probe-read-probe-makes-the-vendors-probe-time-read-then-the-secure-configuration)
gives the vendor trace, the write exception and the meaning of each
result. `check.py --probe-read` runs thirteen cases, and bundle preparation
requires the image's table to hold exactly the fifteen rows.

A wrapper-write variant was refused before any board run and removed
([gpu-power.md](../../docs/gpu-power.md#the-wrapper-write-probe-was-refused)).

## Build and evidence

Run with the existing host Python environment containing pyelftools and Unicorn:

```sh
~/.cache/sunstoneos/venv/bin/python apps/gpu-power-id/build.py --output /absolute/new/build
~/.cache/sunstoneos/venv/bin/python apps/gpu-power-id/check.py \
  --image /absolute/new/build/image.elf --output /absolute/new/client-results.json
```

Add `--power-only` to both commands for the power-only variant,
`--wrapper-read` for the wrapper-read variant or `--probe-read` for the
probe-read variant. The vendor-clock-order and vendor-state variants, whose
controller, monitor and supply copies the default path replaced, were
removed after the vendor-state image read the identity.

`prepare.py` turns a build and its check result into a board bundle for
`tools/prepare-board-bundles.py` (rows 11 to 13, 16 and 17 of
[board-bundles-next.md](../../docs/designs/board-bundles-next.md)). For both
power variants it reads `on_steps` and `off_steps` from the image and
requires the domain-5 (TOP5) ON request before the first CORE6 access, the
last CORE6 access before domain-5 OFF, PLL_GPU0 at 1008 MHz before CORE6
policy 8, the clocks and PLL_GPU0 closed after CORE6 OFF, and no CORE6
delay-row access. The build
records `"variant": "power-only"` and its completion text in `build.json`.
The check refuses an image of the other variant: only the identity variant
defines the identity read counter. Its eleven cases cover the normal path of
two cycles with three snapshots, the ON failure in the first and in the
second cycle, an ON that reaches the deadline, a wrong snapshot-page
translation, the CORE-refusal, partial CORE capture, OFF, retained token,
counted row and unparked failures. A failed or expired ON must take its
snapshot before the failure report, and the snapshot reads exactly its 35
words. Board-bundle preparation requires the normal case to have run two
cycles for the power-only and identity variants. Every case requires that no load,
store or address translation reaches the GPU register window.

The build reuses 23 objects from the frozen provider-readiness candidate.
Fourteen replacements compile the client, GPU platform/native adapter, power
controller, resource manager/native adapter, the current shared TOP owner,
the CPU monitor with its EL3 security setup, the PMIC provider and supply
consumer that admit DCDC4 at 960 mV, and the units whose structures changed
after the candidate was built: the board-bundle power service and its trace
helpers, the counter clock and the PMIC native adapter. The input-object
audit replaces every candidate object whose DWARF defines struct
a733_gpu_power, a733_gpu_platform, a733_power_domains,
a733_power_domain_row, a733_counter_clock or bb_power: eight objects. The
power structure holds the cached CORE6 diagnostics. Every compile unit of
the image must agree on the sizes and member offsets of eleven shared
structures, and those the reused objects still define must equal the
candidate's. A unit that only declares a structure emits no DWARF for it,
so the audit cannot see it; the client's main.c is such a unit for
bb_power, which is why power.c is rebuilt with it. Beyond those eleven, every
structure, union and enumeration that a header defines and two compile units
of the image both define must agree in size and member offsets, or in
enumerator names and values; 96 such types do. The recorded private PMIC
callback-member rename does not change its offset or callback order.

Twelve focused cases execute the linked client, mapping predicate and identity
loads. They cover two cycles of two 32-bit identity loads, unexpected identity, ON/OFF faults,
wrong translation, a read crossing the deadline, backward time, retained token,
counted row, missing provider park, and complete/partial cached CORE refusals.
A seen bit records a completed read, including one whose following clock check
fails; it does not make the observation admissible. Power failures print only
cached CORE values, phase, step, error and last read, then take retained error
return. No new hardware read is made for the report. Power transitions and provider progress
are modeled; these cases do not requalify their physical behavior. Existing
shared-owner deadline and resource evidence is preserved by reference rather
than rerun.

## Physical admission is separate

The proposal needs current ROM identity, DDR and CPU/GIC continuity, cache-off
entry, and authority over the returning platform's 18 MiB RAM interval
`[0x41000000, 0x42200000)`. It uses no accelerator pool. The coordinator grants
RAM disposition and retains the existing supervised failure policy.

PMIC admission requires the inherited S_TWI0 gate/reset, APBS route, PL0/PL1
configuration, settled supplies and excluded PMU writers. GPU admission adds
stable oscillator/REF/PERI0 ownership, closed local GPU gates, asserted reset,
inactive INT95 and exclusive GPU-local authority covering CCU, PLL_GPU0 and
CORE6 policy/PWCR at `0x07066000`, and the user's approval of 2026-10-25 for
DCDC4 at 960 mV and the GPU core at 1008 MHz. TOP5 remains controlled by the shared row. An older
root record granting only TOP5 does not authorize these new CORE writes: the
coordinator must extend the existing GPU-local grant explicitly. No prior GPU boot or command
execution may have occurred in this powered lifetime. These conditions prevent
the identity-only OFF path from retiring an inherited executing device.

The seven parent words and their decoding are specified in
[gpu-resources.md](../../docs/gpu-resources.md). They are RTC/CCU reads and do not
require a GPU engine access. A successful DCDC4 snapshot establishes an observed
960 mV setting only while the coordinator preserves its supply continuity.

After the finite NPU host validates copied/reaped jobs and local stop/quiescence,
it restores the 18 MiB interval and releases the former CPU DRAM allocations.
Powered NPU SRAM is outside that interval. Its domain, rail and SRAM remain
under coordinator authority, with no resumed NPU activity permitted during
this diagnostic. Uncertain NPU completion blocks succession.

This diagnostic starts a fresh ledger; it does not import or prove NPU leases.
It has no domain-4 row and performs no NPU register or SRAM access. Supply
verification reads DCDC2 without changing its setting. Shared clock and gate
operations preserve NPU fields. Provider park retires this image's requests;
it does not release inherited NPU ownership or prove global NPU quiescence.
The coordinator separately authorizes the 18 MiB CPU overwrite interval.

Use the existing retaining queue wrapper and frozen returning-kernel runner
with expected text `COMPLETE GPU power identity: 36.56.104.183 in two ON/OFF
cycles, provider parked`.
Require status zero, normal CPU return and the complete identity lines before
classifying BVNC. A draft proposal or a model pass grants no device access.


The selected BXM source and the architectural PWCR restriction are documented in
[gpu-power.md](../../docs/gpu-power.md). BXM writes CORE without polling it; this
candidate's bounded exact status check is a conservative acceptance condition,
not a promise that hardware will acknowledge. Policy 8 and status 0 with any
PWCR other than the reset value `0x101` is refused before PWCR or local clock
writes. Nothing writes the CORE6 delay row.
The pinned SCP fragment is a host-inspected source clue, not a runtime dependency
or evidence that its boot path executed on either board.

The frozen provider-readiness image `3adfa4ad` remains the first proposed board
test. This later candidate needs a fresh recovered board, current root admission
and explicit CORE authority. It must not reuse operating records from the failed
powered lifetime. Neither candidate has been launched by this offline work.
