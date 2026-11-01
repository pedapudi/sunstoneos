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

## Paced physical acceptance needs a completed capture

Queue entry `1793556850281227875-c450351e` fails before launch on port 3-1.
The first FEL `version` transaction times out during USB bulk send after
10.022193 seconds. The runner records `host-error` at `identity`, with
`launch_attempted=false` and `quarantined=true`. The originating check is
`tools/board.py:Live.sanity`, through `tx('version')`; its transaction log
records exit code 2 and no output. No reset, DRAM setup, image upload or native
execution occurs. There is no application transcript, CRC or pacing timing.

Port 3-1 is paused. Coordinator recovery and resumption are required before a
new control; all five repeat trials remain unsubmitted. The image's numerical
and pacing behavior is untested on this OS revision. An image change cannot
resolve the observed prelaunch transport failure. The runner supplies one
native launch per submission; the planned trials check fresh sessions rather
than five replays within one native session.

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

Fresh images, submission metadata and the transport-failure decode are in
`~/.cache/sunstoneos/build/app-acceptance-1031/queue-1101/`; its
`shared-patches/` directory holds integration patches. Recorded and existing
host parity evidence remain in the sibling `on-split/` directory. Sustained replay, live input,
session expiry and accelerator-assisted execution remain unqualified.
