# Finite GPU return and RAM disposition

The GPU host adapter prepares one returning single-core image and captures its
copied output through the returned A733 ROM service. Preparation checks the ELF
entry, native symbols, immutable memory geometry and the 592-byte session.
The native build identity, epoch, generation, lease and entry digest bind the
launch request and contract to the session. Larger program and texture sessions
are refused before bundle creation.

Prepare a bundle from the repository root:

```sh
~/.cache/sunstoneos/venv/bin/python -B apps/gpu-finite/prepare.py \
  --image IMAGE --session-template SESSION_TEMPLATE \
  --output BUNDLE --clang CLANG --lld LLD
```

The producer prints the actual bundle directory. An existing output directory
receives a unique timestamped child, preserving previous evidence. Optional
`--manifest` accepts build metadata. Optional `--admission CONFIG` reads only
`ram_disposition` and `reservation`; these configure the RAM policy and native
session identity. The default policy restores RAM. Set `ram_disposition` to
`discard` only when both complete RAM ranges contain no data that must survive.
A missing reservation receives a fresh nonzero native identity.

The coordinator selects the physical USB port explicitly and invokes the runner:

```sh
SUNSTONE_USB_PORT=3-3 ~/.cache/sunstoneos/venv/bin/python -B \
  apps/gpu-finite/host.py --bundle BUNDLE --fel FEL --evidence RESULT
```

The shared returning transport holds the per-port lock, validates the live A733
ROM instance and uses the USB transition guard. The adapter directly tests and
restores a reserved DRAM word before snapshots or uploads. Uncertain probe
restoration retains ownership. Each upload uses protected source bytes and
readback. The direct forced-FEL entry must contain the five expected ROM
instructions, and the watchdog must be disabled before launch. The runner
performs one launch and stops all further requests after a transport failure.

Restoration preserves `0x41000000..0x42200000` and
`0x48000000..0x79000000`: 18 MiB of CPU state and 784 MiB of GPU pools.
The adapter saves these ranges in 201 chunks of at most 4 MiB. Every chunk is
size-checked, synchronized and hashed before launch. The smaller allocated
parents do not establish confinement and cannot reduce these envelopes.
Discard mode skips those parent snapshots and restorations.

After a new accessible A733 ROM instance appears, `report.py` validates the
monitor result, platform log, retained receipt and output packet. They must agree
on session identities, successful terminal states, publication bounds, counter
ordering and sequences 1 and 2. Both sets of 32 output words must match the fixed
arithmetic workload. Restoration rechecks every saved chunk, transfers its
protected bytes and compares the readback. Ownership release requires all
restorations to succeed. The returned ROM owns SRAM, so prior SRAM is never
restored over it. Failed or uncertain completion retains ownership for recovery.

Physical execution still requires usable board state, exclusive memory and
resource ownership, initial GPU isolation, and supported settled voltage and
clocks. Native guards enforce their session and hardware conditions. Software
checks establish packaging and modeled failure behavior; they do not qualify GPU
operation. `prepare-check.py` covers packaging without process records, output
preservation and damaged session or upload bytes. `host-check.py` covers reports,
live-memory readiness delegation, ROM guards and full snapshot/restoration bounds
with modeled transport and files.

## Failed physical attempt and input audit

The corrected two-job candidate was attempted on 2026-10-24. Evidence is under
`~/.cache/sunstoneos/evidence/processor-completion-20261024/port-3-3/gpu-corrected-guard/`.
Its `runtime/session.json` records 27 successful FEL transactions, including
`exe 0x52000`, followed by no distinct ROM instance within 90 seconds. The host
retained ownership on that outcome. This establishes execution request delivery; launcher
admission, AArch64 entry, provider construction and GPU execution remain
unobserved.

The offline audit compared the frozen host, preload and launcher with the
candidate's source closure in
`gpu-returning-repeat-fix-20261019/object-source-closure.json`. All nine uploaded
regions matched their frozen bytes on readback. The 592-byte session, 288-byte
contract ABI within its 4,096-byte buffer, and 128-byte request agree on image,
entry digest, epoch, generation, lease and reservation. No binding mismatch was
found. The session SHA-256 is
`a4e48a555995ee49ef3e4f1279e98939fe8a91e016792c6e79badc1b04d6a5ec`;
the bundle records the candidate image and launcher hashes.

The candidate's frozen monitor parks on faults or deadline expiry with timers
disabled. Absence of a ROM return therefore cannot identify the failing native
stage. Its normal return uses RVBAR `0x20` and `RMR_EL3=2`; the
[boot-input guard classification](../../docs/watchdog.md#choose-the-boot-input-guard-from-the-reset-path)
explains why that path has no low-input requirement. The ROM instruction check
runs after preload snapshots SRAM because FEL `readl` executes a helper in
scratch RAM. A raw ROM read is not a verified replacement; the captured-ROM
procedure records its rejection.

The frozen monitor writes result magic `0x4153373347535232`, selected by its
`A733_SMP_RESULT_MAGIC` build definition in `commands.json`. The normal host
parser requires that magic and completion detail `0`. A checkpoint with detail
`1` or a zero-job packet cannot satisfy the normal two-job result contract.
The focused `host-check.py --writer-only` check connects the actual frozen
monitor writer to the host parser. It preserves the unchanged monitor bytes
and labels its arithmetic/log/receipt companions as synthetic.
Evidence is under `~/.cache/sunstoneos/evidence/gpu-monitor-parser-20261024/`.
This corrects a future capture rejection; it cannot explain absence of ROM
before any result capture.

## Historical candidate and bounded physical procedure

The attempted two-job image is preserved under
`~/.cache/sunstoneos/evidence/gpu-returning-repeat-fix-20261019/`:

- Image: `build/return.elf`, SHA-256
  `2ab2cb1b3bac150d5eb695793ab0eb39bb453afc7fbd7ecb2f595ebfca3fd08d`.
- Manifest: `manifest.json`, build identity
  `34572150354cb042d46a5cc9c5830c93f5c60eaae973a5e449f27033b32de5e2`.
- Reused evidence: `root-review.json`, `changed-path-result.json`,
  `client-result.json` and `host/result.json`. The earlier publication and
  host evidence retain the limits described in [RETURN.md](RETURN.md).

The preparation and capture checks default to this corrected image. The older
publication image has a callback that rejects the second submission; its
publication results remain useful, but it is not the two-job candidate.

The candidate starts at `sd_single_core_entry` on the boot CPU, checks that
the seven secondary CPUs remain OFF, and enters the single-core FreeRTOS SRE
port at Nonsecure EL1. Its tasks run at EL1t. It creates no native USB task.
After two jobs, healthy CLOSE and provider quiescence, the EL1 exit shim
publishes the output before SMC enters the GPU return monitor.
The monitor restores interrupts and requests the reviewed ROM reset.
This path has no dependency on the all-core boot barrier. Single-core kernel
and USB results from another image do not establish this candidate's GPU
startup, computation, stop or return.

Each job uses one workgroup of 32 lanes, with unsigned input words `seed=17`
and `factor=3`. The expected words follow the [fixed shader](../../docs/gpu-compute-job.md).
An independent host calculation gives first word `0x9e3779ad`, last word
`0x9e5679c8`, and SHA-256
`a7d48e5bff0ccbb3d2f7557f5b2ba5db5014a958e2fa11a9599c2f7b8b6bd4ef`
for one 128-byte little-endian result. Both jobs produce the same words,
with completion sequences 1 and 2. Concatenating their 256 result bytes gives
SHA-256 `30f134071ed2090211b1e1a68949480afb56a960768775e30cdbba27a54d02c8`.
The return packet, log and receipt must also pass `report.py` before restoration.

Before a physical attempt, the coordinator must bind the candidate to the
current target and establish the [GPU entry facts](../../docs/gpu-application.md).
These include initial isolation and drain, the complete RAM reservations, seven CPUs OFF,
supported settled DCDC4 power, the 400 MHz parent, shared-provider ownership,
remote-master exclusion and the required mappings. The entry records must
also cover the platform validator's ROM USB ownership conditions.
Current ROM continuity requires its recorded cache-off preparation and fresh
survey. A board's earlier observations cannot supply another powered lifetime's
admission. No physical admission is present in the software fixtures.

The port `3-3` records under
`~/.cache/sunstoneos/evidence/board-session-20261023/port-3-3/` provide reusable
facts when the coordinator establishes continuity from their powered lifetime:

| Record | Fact available for admission |
| --- | --- |
| `ddr-2/session.json` | Successful cold training of 12,288 MiB after the recorded power removal. It does not establish exclusive ownership of the GPU reservations. |
| `set5-bb2-run/session.json` | The current single-core kernel completes and returns. The GPU candidate has its own frozen monitor and memory geometry. |
| `set5-r9-v6-usb/session.json` | Native USB takeover, exchange, quiescence and ROM return work with the changed PMIC provider. The finite GPU candidate creates no USB task. |
| `bb6-run/` and `set5-bb6-run/` | PMIC service and DCDC4 supply verification work in this lifetime. Reuse the configured 800 mV evidence with settling and subsequent writer exclusion. |
| `bb8-run/` | NSI priority changes persist. The session must describe those fields and the observed 400 MHz MBUS clock. |

These records avoid repeating kernel, USB, DDR or PMIC checks merely to prepare
the candidate. They do not yet bind its GPU PLL, initial reset/drain, complete
RAM ownership or current launch instance. Those are the remaining physical
admission facts; full multicore operation and watchdog qualification are not
dependencies of this image.
The [entry capture procedure](ENTRY.md) gives the remaining register addresses,
native predicates, access widths and scratch-restoration steps.

Once those conditions are satisfied, the coordinator can reserve one attempt
on an available board while holding its per-port lock.
`host.py` preserves RAM according to the admitted disposition, launches once,
and allows 90 seconds for a new
ROM instance after the execution request returns. The launcher grants a
60-second native deadline; the host adds the shared transport's existing
30-second ROM discovery allowance. This allowance does not extend native
execution authority. The host records the selected window and rejects a node
observed at or after expiry, retaining ownership for recovery. Other shared
transport callers retain their default 30-second window.
Individual memory transfers have 120-second limits and other FEL requests
have 15-second limits. These limits do not bound the total snapshot time.
Success requires both numerical results, normal stop, validated return and
the admitted RAM disposition. Restoration additionally requires all 201
readbacks. The first transport failure or invalid return
retains ownership for supervised recovery and forbids another attempt.
Invoke the runner directly in a persistent process and observe its progress
without a process-killing timeout. Its failure path holds the board lock until
coordinator recovery. A wrapper that kills the runner after 300 seconds would
release that lock despite unresolved device ownership.

Offline evidence for the session-size change is under
`~/.cache/sunstoneos/evidence/gpu-resume-20261024/`: `session-envelope/result.json`
records two larger-envelope refusals and `producer/result.json` records the
actual corrected-image producer and validator. Its newly built launcher has
SHA-256 `7af33010ca810084a5b5dee8f23aa0d16f461f7c34ebcc0bad8f50e779aa18c8`.
That successful producer check models admission; it grants no board authority
and does not execute launcher instructions. Earlier host and native behavioral
checks were reused. No physical test or performance measurement ran.

The return-window change adds no FEL requests or copies. Passive discovery
keeps its 50 ms polling interval: at most 1,800 sleeps over the GPU window,
compared with 600 over the default window. The fake-clock selection in
`apps/npu-finite/transport-check.py --return-window-only` covers late success,
strict expiry, retained failure, invalid configuration and GPU adapter selection.

The default restoration budget remains 802 MiB read, 802 MiB restored and 802 MiB read
back: 2,406 MiB total. At assumed payload rates of 570,000 and 250,000 bytes
per second, transfers take approximately 74 and 168 minutes before overhead.
These estimates are not new physical measurements. Saved and restored files
need at least 1,604 MiB of disk space. The five allocated GPU parents and
cache-publication bounds do not prove complete firmware or device write
confinement, so they do not justify reducing the preservation envelopes.

Focused evidence for the host window is preserved under
`~/.cache/sunstoneos/evidence/gpu-launch-readiness-20261024/`.
`return-window-final/transport-result.json` records fifteen passing cases;
`producer/result.json` records preparation and validation with the unchanged
native image and launcher. The producer uses modeled ownership records and
does not grant physical admission. The prior target history and unresolved
coordinator facts remain in `gpu-finite-admission-20261024/`.

Discarding both complete RAM ranges requires that no other owner needs their
contents. Supply `{"ram_disposition":"discard"}` through the optional
`--admission` configuration. Missing or `restore` mode keeps full preservation;
unknown modes are refused.

Discard mode skips the 201 parent backups and 201 restoration transactions.
The saved transfer is 2,406 MiB, with at least 1,604 MiB less evidence storage.
Protected SRAM capture, immutable upload/readback, both numerical results,
native stop/drain and validated normal ROM return remain required. No old SRAM
is restored. Success records `ram_disposition: discarded`, `dram_restored: false`
and permits ownership release; restoration records `restored` and true.
Failures retain ownership and leave disposition unresolved. Storage is untouched.

Focused evidence under `~/.cache/sunstoneos/evidence/gpu-discardable-20261024/`
records eight host cases and eleven admission/producer cases. The modeled
capture makes 410 transport calls in restoration mode and eight in discard
mode, excluding unchanged protected preload. Both producers retain the same
native image and launcher. Model ownership records grant no physical authority.

Normal return writes RVBAR `0x20` and requests `RMR_EL3 = 2`. The pinned ROM
entry sets `r6 = 0x5c` and branches to the FEL dispatcher, bypassing the normal
boot-input sampler. The host checks the same five instruction words as the
physically exercised CPU return runner. A high boot-input sample does not
refuse this finite profile. Native failures still retain ownership and require
coordinator recovery; this correction supplies no watchdog recovery guarantee.

The instruction check uses the existing FEL `readl` helper, which writes SRAM
scratch. It therefore follows the protected preload and retains ownership on
any refusal. Direct ROM reads are not substituted: the captured board ROM
rejected that access. Recovery from a known pre-launch refusal needs separate
coordinator review; the runner neither restores automatically nor retries.
