# Run the NPU at 852 MHz at the inherited 800 mV

Status: **implemented** as a second PLL_NPU profile, **host-tested** for the
rate change from 492 MHz and **qualified on the board** for the default start
at 852 MHz on port 3-3. The 492 MHz to 852 MHz retune has not run on a board.
Nothing writes DCDC2, so the NPU supply stays at the 800 mV that boot firmware
leaves, and the power-domain owner verifies it. The
[three-start runbook](../board-runbooks/npu-starts.md) runs the retune last in
a lifetime. The profile is rank 6 of the
[vendor action plan](../vendor-firmware/action-plan.md).

## What the vendor table allows

The board kernel's NPU operating-point table (`sun60iw2p1-cpu-vf.dtsi`
lines 2069–2091, linux-orangepi `2ac08e8c`, SHA-256 `d5535630…`) lists
852 MHz with a voltage per speed-bin column:

| Columns | 852 MHz voltage |
| --- | --- |
| vf0000, vf0200, vf0201, vf0400, vf0500, vf0501, in both the 24 MHz and the 26 MHz oscillator columns; 26 MHz vf0204 | 800 mV |
| vf0401 | 820 mV |
| vf0202, vf0203; 24 MHz vf0204 | 880 mV |

The vendor driver selects 1,008 MHz and never changes DCDC2
([NPU backsolve](../reference-backsolve-npu.md#npu-driver-probe), step 11).
Its own table lists 1,008 MHz at 900 mV or more, so that setting runs outside
the table and SunstoneOS does not use it.

## Implementation

- **Profile.** 852 MHz = 24 MHz × 71 ÷ 2: N=71 and P0=2, factor field
  `0x00104600`, a 1,704 MHz oscillator inside the board source's N range of
  53–105. 492 MHz is the other profile.
- **Default.** Without a rate request, the register pass in
  `drivers/npu/a733_npu_startup.c` selects the vendor's point for the speed
  bin: 852 MHz for SID codes `0x12`, `0x04` and `0x05`, and a refusal before
  any write for every other code. This is because a point above 800 mV needs a
  voltage change this startup does not make. Both boards read code 4.
- **Explicit request.** `a733_npu_startup_rate(s, 852)` or `(s, 492)` selects
  a profile after initialization and before the scheduler. The finite image
  requests 852 MHz when built with `apps/npu-finite/build.py --rate-852` and
  492 MHz with `--rate-492`; the OS image requests 492 MHz in its low boot
  mode. An explicit 852 MHz request is admitted for the codes whose columns
  list 800 mV in both oscillator columns: `0x02`, `0x12`, `0x04`, `0x05` and
  `0x15` ([PLL table](../npu-pll.md#two-profiles-and-the-rate-change)).
  Every other code, including an unprogrammed 0, refuses before any write.
- **Rate change.** A PLL left locked at the other profile is retuned in the
  vendor's order: output gate off, factors, lock enable off and on, LOCK,
  output gate on. Each write is read back.
- **Adapter.** The allowlist adapter accepts the 852 MHz factor field with the
  output closed, besides the 492 MHz one.

## Figures

| Quantity | 492 MHz | 852 MHz | Evidence |
| --- | --- | --- | --- |
| Modelled boot to READY, cold enable, domain 4 OFF | 4,628 µs | 4,628 µs | `npu-852mhz-plan.jsonl`, `npu-852mhz-startup-model` |
| Modelled boot to READY, rate change of a running PLL from the other profile | 4,615 µs (from 852) | 4,615 µs (from 492) | same |
| PLL writes in a rate change | 5 | 5 | same |

The host models execute no NPU arithmetic, so they give startup times but no
job time.

## Open work

- Run the three-start runbook on port 3-3: it exercises the adoption of a
  492 MHz PLL and the retune to 852 MHz in one power lifetime.
- Compare job time at 492 MHz and 852 MHz. The prediction (compute time scales
  by 0.58, outputs byte-identical) is the record `npu-852mhz-job-time` in
  `npu-852mhz-plan.jsonl`; the interrupt profile in
  [npu-irq-completion.md](npu-irq-completion.md) times each job.
- Record the speed bin of port 3-1, which is not recorded, before an explicit
  852 MHz request runs on that board.
