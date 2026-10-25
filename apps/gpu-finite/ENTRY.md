# Capture the remaining GPU entry settings

This is a coordinator procedure for the corrected single-core image identified
in [HOST.md](HOST.md#candidate-and-bounded-physical-procedure). It collects
clock and reset settings without entering the gated GPU register window.
The capture supplies observations for admission; it does not establish memory
ownership, historical drain or permission to launch.

## Reuse the existing returning read helper

The existing pinned `sunxi-fel readl` performs a 32-bit load through an A32
scratch helper. The coordinator's current transport implementation is
`~/.cache/sunstoneos/evidence/board-session-20261023/state-capture-readl.py`.
Its source documents the helper, scratch range, register access widths and
failure retention. Reuse its transaction and restoration path with the address
list below. Its existing main routine reads CPU/GIC state, so it does not
collect this GPU list unchanged. The older `gpu-clock-read-20261018/gpu-clock-run.py`
is bound to an obsolete USB instance and must not be invoked unchanged.

Freeze the selected address list and transport sources under a new existing
queue proposal for port `3-3`. Bind the claim to the actual current instance
and its reviewed recovery record. The coordinator runs the frozen capture
directly, without a wrapper that kills the process on timeout.

1. Hold the existing per-port `board_lock()`. Require session access, usable
   ROM state, unchanged instance identity and the pinned FEL executable hash.
2. Save and synchronize the 128 scratch bytes at `0x71000`. A short snapshot
   stops the capture before any `readl` execution.
3. Issue one batch of the eleven 32-bit reads below, with the existing
   30-second transaction limit. Require eleven parseable unsigned words.
   Save the ordered addresses, raw replies, node and instance identity.
4. Restore the same scratch bytes with `sealed_source`, read them back and
   require byte equality. This writes only the helper's SRAM scratch area.
   It performs no peripheral-register write.
5. Stop after the first transport, identity, report or restoration failure.
   Quarantine that instance and retain the lock through `hold_ownership()`.
   Do not retry or issue restoration commands after a transport failure.

The existing helper can change ROM instruction-cache state. Before a later
native launch, use the established returning cache-off helper and a fresh
launch survey, then bind that continuity into admission. Do not repeat DDR
training in a continuing warm ROM lifetime.

## Read eleven configuration words outside the GPU window

The source basis is the frozen candidate's `a733_gpu_resources.c`,
`a733_gpu_resources_native.c` and `a733_clock_reference.h`, recorded in
`gpu-returning-repeat-fix-20261019/object-source-closure.json`.
The corresponding [resource contract](../../docs/gpu-resources.md) identifies
the pinned A733 manual sections and address meanings. These RTC and CCU
configuration/status words remain accessible while the GPU is reset and gated;
none is an interrupt acknowledgment or a clear-on-read register.

| Address | Symbol below | Required predicate |
| --- | --- | --- |
| `0x07090160` | `osc` | `(osc & 0x10000002) == 0x10000002` |
| `0x02002000` | `ref` | `(ref & 0xf8000000) == 0xf8000000`, plus reference decoding below |
| `0x020020a0` | `peri` | `(peri & 0xfe000002) == 0xfe000000`, plus parent decoding below |
| `0x020020a8` | `sdm0` | `(sdm0 & 0x80000000) == 0` |
| `0x020020ac` | `sdm1` | `(sdm1 & 0x08000000) == 0` |
| `0x02003908` | `branches` | `(branches & 0x08060806) == 0x08060806` |
| `0x02003988` | `effective` | `(effective & 0x08060000) == 0x08060000` |
| `0x02002b20` | `local_clock` | `(local_clock & 0x80000000) == 0` |
| `0x02002b24` | `local_reset` | `(local_reset & 0x00010001) == 0` |
| `0x020025c0` | `ahb` | `(ahb & 0x80) != 0` or `(ahb & 0x0100007f) == 0x0100007f` |
| `0x020025e0` | `mbus` | `(mbus & 0x10000) != 0` or `(mbus & 0x41045800) == 0x41045800` |

Decode the oscillator selector `(osc >> 14) & 3` as 24 MHz, 19.2 MHz,
26 MHz or 24 MHz for selectors 0, 1, 2 or 3 respectively. Let
`Nref=((ref >> 8) & 255)+1` and `Mref=((ref >> 16) & 127)+1`.
Require `80 <= Nref <= 116` and
`oscillator_hz * Nref == 24000000 * Mref`.
For the GPU parent, let `Nperi=((peri >> 8) & 255)+1` and
`Mperi=((peri >> 20) & 7)+1`. Require `53 <= Nperi <= 105` and
`24000000 * Nperi == 400000000 * Mperi * 3`.
Use integer arithmetic. These equalities establish configured rates, not
physical frequency measurements.

An already enabled GPU bus gate is accepted by the native resource manager.
When that gate is disabled, the non-GPU key bits must already be set before
the manager enables it. The capture must not set them. The PCK gate word
`0x070101ac` is optional context: native acquisition permits either initial
bit-0 value and enables it when needed. No PCK-domain register read is needed.
Do not read GPU revision, firmware, cache or idle registers at `0x01800000`;
native startup accesses that window only after its power and clock admission.

## Bind existing state to the new capture

Reuse the port's DDR, kernel, PMIC and USB evidence listed in [HOST.md](HOST.md).
The complete power removal before `ddr-2` and the subsequent operation history
can establish an initial isolated lifetime if the coordinator confirms no
GPU activity or unresolved DMA operation occurred afterward. Clock/reset
readback alone does not establish that history. Reuse the actual DCDC4
800 mV observation with settling and exclusion of later rail writers.

Use existing CPU-OFF observations when their continuity is established.
Otherwise append the existing read helper's 21 CPU-power words: for each
`c=1..7`, read `0x07052008+c*0x1000`, `0x07050200+c*4` and
`0x07050220+c*4`. Require a zero low nibble in the first and bit 0 clear in
the other two. These are the frozen single-core monitor's `secondaries_off`
predicates. Reading them starts no secondary CPU. No GIC or CPU configuration
window capture is needed solely to establish those predicates.

The coordinator then records exclusive ownership of
`0x41000000..0x42200000` and `0x48000000..0x79000000`, shared-provider and
remote-writer exclusion, and the reviewed mappings. Construct the fixed
592-byte session from these actual facts, including the continuing 400 MHz
MBUS configuration and any retained fabric settings. Bind the new capture,
reused records, image, launcher and host source closure through the existing
entry and return-review schemas. The capture cannot manufacture those ownership
facts. Successful offline preparation then makes the concrete launch bundle
reviewable; the coordinator decides physical eligibility from the bound evidence.
