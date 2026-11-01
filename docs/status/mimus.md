# Mimus application acceptance

Mimus runs through the SunstoneOS application kit from its own repository.
The acceptance branch `port/sunstoneos-acceptance-on-split` is based on CPU
engine revision `0cbc96950`. Commits `1a485bad4` and `8f0e0f127` supply paced
replay and CPU-only manifest permissions. The separate build patch selects
paced replay. Images use SunstoneOS `e124d65f`; its cached C++ layout includes
the CPU worker's fix. No duplicate linker patch is required.

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

Queue entry `1793516482062422295-b7b2893e` has no final result or captured
transcript. Its board outcome and timing are unknown. The stopped queue daemon
and unavailable boards block physical work. The coordinator must reconcile
that interrupted entry and recover board access before another submission.
A paced control and five subsequent fresh-session trials remain required.
The runner supplies one native launch per submission; these trials do not
exercise retained application state across five replays in one native session.

## Host validation passes

OS-only, hello-compute and recorded application baseline builds pass. The
paced image also passes linked-image and public-interface checks. Actual kit
adapters with eight FreeRTOS workers pass recorded and paced QEMU parity.
The QEMU kit grant models five regions; physical recorded admission checks the
production manifest's six regions, including packed weights.
Both adapter sanitizer checks pass. The selected integration gate and check
index pass. Independent review finds no blocking rebase or build-patch defect.
Firmware and host sources equal the requested engine revision.

Builds, source identities, image READMEs, transcripts and parity records are in
`~/.cache/sunstoneos/build/app-acceptance-1031/on-split/`. Shared integration
patches and the exact remaining submission commands are in that directory's
`shared-patches/` and `remaining-submissions.sh`. Sustained replay, live input,
session expiry and accelerator-assisted execution remain unqualified.
