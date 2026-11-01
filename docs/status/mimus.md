# Mimus application acceptance

Mimus runs through the SunstoneOS application kit from its own repository.
The acceptance branch `port/sunstoneos-acceptance-on-split` is based on CPU
engine revision `0cbc96950`. Commits `1a485bad4` and `8f0e0f127` supply paced
replay and CPU-only manifest permissions. The separate build patch selects
paced replay. The recorded result uses SunstoneOS `e124d65f`; the fresh paced
image uses `cbf591ff`, including `bringup` `6ebe687a`. The cached C++ layout
includes the CPU worker's fix. No duplicate linker patch is required.

## Recorded input passes physical acceptance

Queue entry `1793516356151336408-cac6790a` on port 3-1 returns normally with
seven secondary CPUs OFF and complete reference parity. The transcript is
`There was silence.` Feature CRC is `ac657a3b`, probability CRC is `a7ade80e`,
and the 57,440-sample recording produces 90 frames.
Recognition reports `compute_us=135673` and `total_us=135715`; loading and
packing take `92095` microseconds before recognition. Packed weights occupy
13,496,816 bytes. The compute duration is below the reference's 179.5 ms limit.
This single observation does not qualify corpus latency or thermal behavior.

The image reports CPU-L/B at 1794/2002 MHz and 1000/1050 mV, with three
completed domain transitions. The printed DSU rate of 494 MHz is the admission
snapshot from `apps/os/cpu.c:mhz`; it does not measure the active DSU clock.

## Paced recognition passes; thermal capture remains incomplete

Queue entry `1793558023604334567-7e722543` on 3-1 returns normally with seven
secondary CPUs OFF and exact segmented reference parity. It processes all
57,440 samples and consumes one segment spanning `[19520, 57440)`. Feature CRC
is `efcd0911`, probability CRC is `c78d3801`, and formatting produces
`There was silence` followed by flush `.`. Recognition takes `92684`
microseconds; replay takes `3683197`, maximum lateness is `445`, and latency
from speech end is `93166` microseconds. Loading takes `92091` microseconds.

The runner outcome is `result-mismatch` at capture. Its thermal window has
36 fresh rows, no overflow and successful recognition, but three adjacent
row gaps are 125002, 125003 and 125002 microseconds. These exceed the
unchanged 125000-microsecond completeness bound. The originating check is
`apps/os/thermal-report.py:classify`; `tools/board_images.py:capture` marks
that receipt invalid. The application completion text matches its expectation.
CPU retirement and rail cleanup succeed; the instance is not quarantined.

Commit `e8887a23` implements absolute 25 ms sensor deadlines and a 125 ms
report phase. Version 2 headers carry the first selected scheduled time through
USB and returning capture. Each sample must match its consecutive scheduled
slot within 2001 microseconds: one 1 ms wakeup tick, one 1 ms read/publication
budget and one microsecond of timestamp quantization. Longer lock, read or
scheduling delays fail completeness; this is an acceptance budget rather than
a worst-case execution guarantee. Missing slots remain failures. Version 1
reports retain their earlier decoding behavior.

Eight native cases execute the actual owner step and publication with modeled
THS completion and policy values. They cover 1 ms polling, opposite completion
jitter, lateness boundaries, IRQs, missed periods, critical and empty windows.
Nine decoder cases check lateness, missing samples and both report transports.
The OS baseline and archived linked capture pass. Independent review approves
the committed code. The queue must load the updated decoder before version 2
board submissions; the new paced control and repeat batch remain unqualified.

The five paced repeats remain unsubmitted. A new control must first pass the
complete capture check. Each trial uses a fresh native session; these trials
do not qualify five replays within one retained application session.

## Host validation passes

OS-only, hello-compute and recorded application baseline builds pass. The
paced image also passes linked-image and public-interface checks. Actual kit
adapters with eight FreeRTOS workers pass recorded and paced QEMU parity.
The QEMU kit grant models five regions; physical recorded admission checks the
production manifest's six regions, including packed weights.
Both adapter sanitizer checks and QEMU parity retain their evidence for the
unchanged application sources. Fresh baseline builds, the CPU clock model,
three returning-watchdog cases, the selected integration gate and check index
pass on the updated OS. Independent review finds no blocking rebase or build-patch defect.
Firmware and host sources equal the requested engine revision.

Fresh images, submission metadata, parity and fault-origin records are in
`~/.cache/sunstoneos/build/app-acceptance-1031/queue-1101/`; its
`shared-patches/` directory holds integration patches. Recorded and existing
host parity evidence remain in the sibling `on-split/` directory. Sustained replay, live input,
session expiry and accelerator-assisted execution remain unqualified.
