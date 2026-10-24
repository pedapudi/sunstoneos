# Prepare one NPU inference and normal ROM return

This single-core image runs two jobs using its immutable model and input,
compares both outputs, reaps them, closes the NPU service and gates its local
clocks and asserts engine resets. It parks the power service only after requests,
borrows and mutexes are idle. The monitor then restores interrupts and returns
to ROM. The shared NPU SRAM, domain 4, PLL and rail remain powered and owned
through that return. No power-off or in-place NPU restart is established.

The host saves the entire 18 MiB interval `0x41000000..0x42200000` before
uploading. It restores and compares that interval only after a responsive new
ROM instance supplies matching completion reports and copied numerical output.
The native limit is 20 seconds; ROM discovery is bounded by 30 seconds.
A failed or uncertain attempt retains ownership for physical recovery.

The SMP USB thermal image is a separate consumer. Its local CLOSE leaves
USB active until its 20-second deadline and supplies no normal ROM return.
The finite image currently has no thermal admission or sensor-stop integration.
Numerical return qualification therefore cannot establish thermal behavior.

## Construct the session from explicit physical inputs

Use the exact image and manifest together. The `session` operation takes an
already reviewed admission record and the complete typed boot bytes. It
serializes their identities and hashes, validates the result with the existing
admission predicate, and refuses an existing output file. It supplies no
ownership, clock, electrical or recovery defaults.

The compiled session layout is read from the image. For the current producer,
the session has 456 bytes and the typed boot payload has 232 bytes. Within
that payload, the nine profile identities occupy bytes 0–71, fabric words and
rates occupy 72–95, PMIC configuration occupies 96–127, and the pin handoff
occupies 128–231. The layouts are defined by `a733_npu_application_boot`,
`a733_pmic_config` and `a733_pin_handoff`; preserve zero padding and the zero
uploaded PMIC deadline. Supply the actual inherited PL0/PL1 settings and
explicit pin authority. A software fixture is not a physical boot payload.

```sh
python apps/npu-finite/host.py session --image IMAGE --manifest MANIFEST \
  --boot TYPED_BOOT --admission ADMISSION --output SESSION
python apps/npu-finite/host.py prepare --image IMAGE --manifest MANIFEST \
  --session SESSION --admission ADMISSION --output BUNDLE \
  --clang CLANG --lld LLD --objcopy OBJCOPY
python apps/npu-finite/host.py validate --bundle BUNDLE
```

The admission must bind the current target and continuity, the complete DDR
reservation, secondary CPUs OFF, cache and mapping authority, independent
DRAM clocks, NPU SRAM exclusion and drain ownership, PMIC pin ownership and
stable DCDC2 at 800 mV. The separate returning-lifetime review binds the exact
image, launcher and host sources. It authorizes retirement only after verified
CLOSE, local stop, provider quiescence and ROM return. The power domain's
coherent ON observation does not supply any of those authorities.

## Verify the actual forced-ROM entry

The linked return tail writes RVBAR `0x20` and RMR `2`. That ROM entry sets
`r6` to `0x5c` and branches through `0x22c` to `0x2514`, bypassing the normal
boot-input branch at `0x10134`. The normal boot GPIO is therefore not a
precondition for this return.

After protected preload has saved SRAM scratch, the host reads five pinned ROM
instructions in one `readl` batch. Addresses `0x20`, `0x24`, `0x28`, `0x3c`
and `0x40` must contain `0xeb00008f`, `0xe3a00911`, `0xe3a0193d`,
`0xe3a0605c` and `0xea00004d`. Any mismatch refuses execution. The same
instance, port, disabled watchdog and admission checks still apply. `readl`
uses 48 bytes of scratch at `0x71000`; raw FEL reads of ROM are not a substitute.
The source-backed ROM capture is preserved in
`~/.cache/sunstoneos/evidence/gpu-rom-entry-20261024/snapshot/manifest.json`.

## Freeze before the coordinator claims the board

Preserve every `HOST_SOURCES` entry, all runtime imports and the external USB
guard with the image, manifest, actual launcher and complete native compiler
input closure. Build and import the frozen producer offline. Supply current
DDR and survey evidence plus every accepted warm return in the continuity
chain. The run command uses the existing root-owned claim and never creates
or closes queue records:

```sh
python BUNDLE/host/apps/npu-finite/host.py run --bundle BUNDLE --fel FEL \
  --evidence RESULT --ddr-evidence DDR --platform-survey SURVEY \
  --warm-return ACCEPTED_RETURN --root-owned-claim CLAIM
```

Stop at the first transport failure. A deadline does not permit another
request, memory restoration or claim release. After normal return, old SRAM
belongs to ROM and is never restored. Another NPU start still requires the
startup contract, including a disabled PLL; successful return does not itself
satisfy that contract.
