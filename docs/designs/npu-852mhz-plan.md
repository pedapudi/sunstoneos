# Run the NPU at 852 MHz at the inherited 800 mV

Status: implemented as a second PLL_NPU profile, checked in the host model
and on the linked instructions of the finite image built with `--rate-852`.
It has not run on a board. The user approved NPU clock changes on
2026-10-25, 852 MHz first at the current 800 mV, as the `bringup` handoff
records (`3c17aeca`);
NPU voltage changes are not approved, and nothing writes DCDC2. The
[three-start runbook](../board-runbooks/npu-starts.md) runs it last in a
lifetime. This is rank 6 of the [vendor action plan](../vendor-firmware/action-plan.md).

## What the vendor table allows

Every earlier SunstoneOS NPU image runs PLL_NPU at 492 MHz. DCDC2, the NPU
supply, stays at the 800 mV that boot firmware leaves; the power-domain owner
verifies it and nothing writes it.

The board kernel's NPU operating-point table (`sun60iw2p1-cpu-vf.dtsi`
lines 2069–2091, linux-orangepi `2ac08e8c`, SHA-256 `d5535630…`) lists
852 MHz with a voltage per speed-bin column:

| Columns | 852 MHz voltage |
| --- | --- |
| vf0000, vf0200, vf0201, vf0400, vf0500, vf0501, in both the 24 MHz and the 26 MHz oscillator columns; 26 MHz vf0204 | 800 mV |
| vf0401 | 820 mV |
| vf0202, vf0203; 24 MHz vf0204 | 880 mV |

The vendor driver itself selects 1,008 MHz and never changes DCDC2
([NPU backsolve](../reference-backsolve-npu.md#npu-driver-probe), step 11).
Its own table lists 1,008 MHz at 900 mV or more, so that setting runs outside
the table, and SunstoneOS does not copy it.

## Change

- **Profile.** 852 MHz = 24 MHz × 71 ÷ 2: N=71 and P0=2, factor field
  `0x00104600`, a 1,704 MHz oscillator inside the board source's N range of
  53–105. `a733_npu_startup_rate(s, 852)` selects it after init and before the
  scheduler; the finite image calls it when built with
  `apps/npu-finite/build.py --rate-852`, which gives the image its own build
  identity.
- **Admission by speed bin.** The register pass reads SID word `0x0300624c`
  and admits 852 MHz only for the codes whose columns list 800 mV in both
  oscillator columns: `0x02`, `0x12`, `0x04`, `0x05` and `0x15`
  ([PLL table](../npu-pll.md#two-profiles-and-the-rate-change)). Every other
  code, including an unprogrammed 0, refuses before any write. Port 3-3 reads
  code 4 (vf0400). Port 3-1's bin is not recorded, so that board needs the
  read of action plan rank 4 first.
- **Rate change.** A PLL left locked at the other profile is retuned in the
  vendor's order: output gate off, factors, lock enable off and on, LOCK,
  output gate on. Each write is read back.
- **Adapter.** The allowlist adapter accepts the 852 MHz factor field with the
  output closed, besides the 492 MHz one.

## Measurement and prediction

The host models execute no NPU arithmetic, so they give startup times but no
job time.

| Quantity | 492 MHz | 852 MHz |
| --- | --- | --- |
| Modelled boot to READY, cold enable, domain 4 OFF | 4,628 µs | 4,628 µs |
| Modelled boot to READY, rate change of a running PLL from the other profile | 4,615 µs (from 852) | 4,615 µs (from 492) |
| PLL writes in a rate change | 5 | 5 |
| Predicted job time on the board, publication to completion observation | one poll period, under 1 ms | one poll period, under 1 ms |

The finite client logs each job's publication and completion observation in
counter microseconds, and the host reports them. The service worker polls
for completion once per 1 ms tick in this polled profile, so a job shorter
than a tick reads as the time to the next poll at either rate. The NPU's
compute time is predicted to scale by 492/852 = 0.58, less where memory bounds
the work; resolving that on the board needs a finer completion measurement
than the tick poll. The outputs are predicted to be byte-identical at both
rates.
