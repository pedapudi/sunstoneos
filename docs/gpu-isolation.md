# GPU memory isolation still requires a physical drain guarantee

The native GPU backend has no qualified implementation that blocks new GPU
memory requests and drains previously issued transactions. The pinned sources
provide firmware shutdown, GPU idle checks, clock/reset control, and power-domain
acknowledgements. They do not establish how A733 acknowledges completion of GPU
transactions already accepted outside the GPU. The external isolation provider
therefore remains mandatory; power OFF cannot substitute for its contract.

Source investigation found a separate error in the provider boundary. A nonzero
failure return could be treated as success after partial power teardown. The
backend now requires a return value of exactly one before reclaiming firmware
memory. This correction is implemented and instruction-model checked. It does
not implement physical isolation.

## Available evidence stops short of interconnect completion

The existing [shutdown controller](gpu-stop.md) closes submission, obtains separate
forced-idle and power-off firmware acknowledgements, retires normal queue/control
access, polls GPU idle state, and asserts soft reset. Its source plan remains
unchanged. The relevant observations are:

| Mechanism | Established source meaning | Missing guarantee for buffer reuse |
| --- | --- | --- |
| GPU BIF/MMU status | Pending translation/request fields and external read status are polled to zero | Completion of every external read and write across the A733 fabric |
| GPU system-level cache status | `SLC_STATUS1` contains read counts; `SLC_IDLE` reports internal idle blocks | The point at which an earlier write can no longer reach backing memory |
| MIPS firmware-wrapper idle | The firmware processor reports idle before reset | Coverage of requests that have left that wrapper |
| GPU soft/external reset | GPU logic enters the selected reset state | Ordering and disposition of transactions already accepted by another block |
| GPU MBUS clock gate | The GPU memory-bus clock can be disabled | Request exclusion plus an acknowledged drain while downstream clocks remain usable |
| GPU_TOP power status | The power controller reports the selected domain state | Which GPU/interconnect interfaces participated in its handshake |

The GPU register definitions name MMU pause controls in `BIF_CTRL` at `0x12a8`
and a paused status in `BIF_MMU_STATUS` at `0x12d0`. The inspected shutdown paths
do not use them as a platform isolation sequence. Their names alone do not prove
coverage of direct firmware accesses, cache writeback, or every GPU master.
Programming those bits without that contract would add an unverified path.

The board NSI driver identifies the GPU as master 6 at the interconnect base
`0x02020000`. Its exposed controls select priority, input/output quality of
service, bandwidth limits, and measurement counters. The master-specific device
also references GPU_TOP for runtime power management. No inspected function
provides a GPU request-block acknowledgement followed by outstanding-transaction
drain. Disabling a measurement counter or bandwidth limiter would not establish
those conditions.

The A733 manual identifies GPU MBUS gate bit 16 at clock-controller offset
`0x5e0`. It describes clock gating, without defining an external transaction-drain
acknowledgement. Shared memory-bus, NSI, and DDR resets or clock changes also affect
other masters and cannot be used as an unowned GPU isolation operation.

## Power-controller architecture requires A733 integration details

The [Arm PCK-600 reference manual](https://documentation-service.arm.com/static/61fa619e2dd99944d0514955)
explains that the Power Policy Unit coordinates device quiescence and power-state
changes. Its device interface can use one P-Channel or several Q-Channels.
Routing, interface configuration, and some state mappings depend on the SoC
integration. A quiescence response describes the connected device's contract;
it does not identify the A733 GPU's complete memory path.

The manual lists power status, device-interface status, and identification
registers. These could help identify a physical implementation after independent
recovery is qualified. Reading them cannot establish which bridges or queues
participate in GPU_TOP shutdown. The pinned board power driver requests state
zero and polls domain status; it provides no additional A733 wiring guarantee.

The missing primary evidence must establish all of these points:

- Which interface blocks new GPU requests, including firmware and cache traffic.
- Which acknowledgement covers accepted reads, write data, responses, and any
  buffered writes that could still reach memory.
- Which clocks and resets must remain active until that acknowledgement arrives.
- How GPU_TOP's P-/Q-Channel and isolation controls connect to the GPU and NSI
  path, or which independently documented per-master sequence supplies the guarantee.
- How the platform preserves unrelated masters and keeps the completed isolation
  valid until buffers are reused or the next controlled power lifecycle begins.

A GPU-local idle sample can be one prerequisite. The callback needs the complete
sequence and its completion semantics before it can authorize memory reuse.

## The next integration must preserve lifetime and scheduling

The current backend calls the isolation provider after soft reset and requires
platform OFF with its token released before reclaiming firmware memory. If the
physical protocol requires pre-reset observations, shutdown must add that stage
before reset. Reset-cleared counters cannot replace evidence collected while
transactions were still observable.

The native power binding already provides bounded off-begin/off-poll operations.
The current external isolation callback is synchronous. Production integration
needs a worker-driven asynchronous isolation operation that retains the absolute
CLOSE deadline across request blocking, drain acknowledgement, and power-off.
It must not spin through power polls inside that callback.

During the operation, the service must keep submission closed, interrupts masked
where required, and all firmware, workload, queue, and page-table backing pinned.
It must retain the GPU resource token and common clock leases until the proven
sequence permits their release. Every shared-register mutation must use the
existing platform register lock and preserve other masters' fields.

A timeout, uncertain I/O result, or unexpected status must retain memory and stop
further callbacks. Resource bookkeeping must reflect any release that already
completed; retaining memory does not undo a provider's partial power teardown.
Only an exact successful provider result, completed power-off, valid ownership
state, and an unexpired deadline permit the backend's existing reclaim step.

No placeholder isolation driver or header is supplied. Existing interfaces keep
the missing physical guarantee explicit. Independent watchdog/ROM recovery and
an integrated test image are prerequisites for coordinator-only qualification;
this investigation creates no board-test request.

## Source identities and focused validation

The board sources use revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`:

- `bsp/drivers/gpu/imagination/pvr_fw_startstop.c` and `pvr_rogue_cr_defs.h`
  define the existing shutdown polls and register fields.
- `bsp/modules/gpu/img-bxm/linux/rogue_km/services/server/devices/rogue/rgxstartstop.c`
  and `services/server/devices/rgxpower.c` supply the additional GPU shutdown reference.
- `bsp/drivers/pm_domain/pck600_domains.c` supplies domain requests and status polling.
- `bsp/drivers/bus/sunxi-nsi.c`, `sunxi-nsi.h`, and `sunxi-nsi-bw.c`, together with
  `bsp/include/sunxi-nsi.h`, supply the available interconnect controls.
- `arch/arm64/boot/dts/allwinner/sun60iw2p1.dtsi` identifies NSI addresses,
  GPU master 6, and its GPU_TOP relationship.

The A733 User Manual V1.00 is the existing pinned primary hardware reference.
Its SHA-256 is
`cdd77e5779d3c69d41b5f237f3db3db51c67fdf0f3aca9a2eedc77d9420f248d`.
The downloaded Arm manual is document `101150_0005_01_en`, issue 01, revision r0p5.
It describes the architecture; it does not identify the instantiated A733 revision.
All downloaded software sources retain their original license notices in the cache.

The focused native check supplies provider results minus one, two, and one after
modeled power teardown. Only one permits reclaim. The other results preserve
published firmware backing, latch an isolation error, and prevent another provider
call. The fixture models the provider and does not claim physical block/drain.
Existing shutdown, power, mode, and completion checks were not repeated.

Evidence resides in `~/.cache/sunstoneos/evidence/gpu-isolation-20261016/`.
It contains the exact executed inputs, native ELF, command log, source manifests,
source-lock proposals, and the result. The narrow entry is
`checks/a733-gpu-isolation.c`; `checks/a733-gpu-isolation.py` reuses the existing
native instruction/translation model and selects only that entry.
