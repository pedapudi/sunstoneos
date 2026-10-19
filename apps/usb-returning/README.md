# Bounded native USB observation with ROM return

This diagnostic runs the production PMIC and USB providers as two FreeRTOS tasks on the accepted returning single-core platform. It takes over an admitted ROM USB0 PIO lifetime, enumerates a vendor interface, copies a 40-byte response, and returns through the existing secure monitor. It initializes no GPU or NPU hardware. Software checks pass; physical enumeration, transfers and return for this composition remain unqualified.

The native observation ends 1.4 seconds after application setup begins. Provider ownership remains live for 1.8 seconds, allowing PMIC parking and the 150 ms detach interval before the unchanged monitor's two-second deadline. The short observation window may produce a controlled negative before the host can connect. That result does not admit a retry.

The host sends the four bytes `USB?` to bulk OUT endpoint `0x02`. Bulk IN endpoint `0x81` returns five little-endian 64-bit words: magic `0x3154455253425355`, input generation, status 1, USB preparation step, and PMIC stage. Native success requires the IN completion callback to account for all 40 copied bytes. This response length avoids TinyUSB's trailing zero-length packet behavior at 64-byte and 512-byte packet boundaries. Host success also requires matching generation, native outcome 0, received count 1, sent count 40, transferred count 40, quiescence 1, and a responsive new ROM instance.

After observation, the USB task requests PMIC parking and waits for its acknowledgment. It requires idle PMIC request slots, no rail borrows, no retained provider fault, and unheld register/I2C mutexes. It checks USB interrupts disabled and inactive, stops native USB polling, masks controller endpoint/bus/DMA/interface interrupt enables, removes soft-connect, and waits 150 ms. Final checks include pending interrupts clear and all sixteen internal DMA channel start bits clear. Controller W1C status bits are preserved. The USB task performs no further TinyUSB work after starting shutdown.

Outcome 0 means the native response completed. Outcome 1 means observation expired without completing that response. Both normal outcomes publish `COMPLETE native USB bounded observation` only after quiescence and exit with platform status 0. Provider, malformed-request, transfer-accounting and shutdown faults exit with status 1 and omit that marker. A monitor timeout or controlled negative does not establish successful USB operation. Clock, rail, pin, provider and buffer ownership stays retained through CPU reset; there is no resource-release or restoration API.

## Source-backed return and its limits

The reused monitor writes the reset-vector base address, RVBAR, as `0x20` and requests an AArch32 warm reset with RMR=2. Address `0x20` is the captured secure ROM's forced-FEL entry. The captured path reaches dispatcher `0x2514`; instructions at `0x2580` and `0x1061c` enter nonsecure ROM at `0x17000`.

For the matched nonsecure ROM with USB selector zero at `0xf355c`, offsets `0x5a8` through `0x5bc` select USB0. Offset `0x2498` calls initialization at `0xa20`. That sequence asserts device reset and disables its gate at `0x948`, asserts PHY reset at `0x9a8`, then releases device reset and enables its gate at `0x968`. Setup at `0x1cf0` initializes PHY, endpoint FIFOs and connection. Return therefore uses ROM initialization of USB0; it does not restore saved endpoint registers.

The secure ROM binary has SHA-256 `a8e31821e8a1611d75867825914c58373c732742cb8f2235e8168c1d7e93b5aa`. Its disassembly is `~/.cache/sunstoneos/evidence/rom-copy-20261016-084055/physical-rom.dis`. The separate `sources/brom/fixs-brom.dis` does not match that captured implementation. The nonsecure ROM has SHA-256 `95be69c916846a8259578565f3193a6b9ab0625294d5374f3dc6c2f667fc6169`. See [ROM takeover provenance](../../docs/usb-platform.md#rom-transfer-provenance-required-for-takeover) for transfer and DMA anchors.

The reviewed ROM and TinyUSB paths transfer USB data through CPU FIFO copies without enabling DMA. Interrupt masking and disconnect do not establish DMA drain. Clear channel start bits support return only when the entire inherited and native USB lifetime is known to have used PIO. External DMA exclusion and completed earlier DMA remain separate physical prerequisites. A secure-monitor timeout can interrupt a PIO copy without leaving a USB engine accessing application RAM, but it does not establish clean shutdown or completed transport.

The existing patched TinyUSB implementation is reused byte-for-byte from `gpu-transport-only-20261018`. Its actual configuration is `platform/a733-storage-update/tusb_config.h`: no TinyUSB OS layer, 2048-byte vendor FIFOs, high-speed PIO and four task events per run. The image recompiles its native USB provider against the returning platform's FreeRTOS configuration. It reuses accepted monitor, platform, kernel, PMIC, clock, rail and pin objects from `pmic-provider-probe-20261018`. Source snapshots, tool identities and object hashes remain in the evidence directory.

## Build and focused checks

Run from the repository root:

```sh
python3 apps/usb-returning/build.py --evidence ~/.cache/sunstoneos/evidence/usb-returning-20261018
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python apps/usb-returning/check.py --evidence ~/.cache/sunstoneos/evidence/usb-returning-20261018
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python apps/usb-returning/host-check.py --evidence ~/.cache/sunstoneos/evidence/usb-returning-20261018
```

Fifteen linked cases execute actual input copying, task construction, production provider/resource constructors, response accounting, and native shutdown MMIO guards. Their configuration comes from an existing session producer's retained board fields. Scheduler behavior, hardware provider progression, TinyUSB FIFO events and register effects are modeled. Twenty-three host cases check one-attempt capture, disappearance, early admission cutoff, transport failure, result consistency and the kernel runner's lock/deadline integration. Earlier passing provider and TinyUSB checks are reused. These checks establish no physical operation.

The diagnostic input producer reads existing board-configuration JSON fields `fabric`, `fabric_hz`, `usb_supply_uv` and `pmic_expected`. The first three contain 3, 3 and 4 unsigned words; the last contains two three-word pin configurations. It emits a 96-byte diagnostic input with an explicit generation. This input carries configuration, not physical authority. The checker-generated fixture is not a board admission record.

```sh
python3 apps/usb-returning/prepare.py --config BOARD_CONFIG.json --generation GENERATION --output INPUT.bin
```

## Coordinator physical prerequisites and one attempt

Only the coordinator operates the board. Before admission, establish the selected physical port/current identity, reconnect permissions, contiguous DDR evidence, current launcher/cache survey, secondaries OFF, complete linker-owned RAM preservation, and the monitor's existing GIC/timer ownership. Establish exclusive USB0/PHY/reference/CCU and PMIC/pin/rail ownership, accessible Device mappings, settled supplies matching the configuration, disabled/nonactive USB INTIDs 188 through 190, and absence of other controller or firmware writers. Match both ROM hashes and selector zero. Review the actual powered-lifetime command history for PIO-only USB and establish external DMA exclusion/drain separately. Preserve both microSD cards. Record a supervised manual power-removal recovery plan.

The image owns DRAM from `0x41000000` through `0x41044abf`, including BSS and stacks. Preserve the page-rounded interval through `0x41044fff`, the result region at `0x42000000`, and the 96-byte input at `0x43000000`.

Freeze the image, 96-byte input, host tool and evidence before the attempt. Use the frozen USB runner with the same admission arguments as the accepted kernel runner, plus these arguments:

```sh
SUNSTONE_USB_PORT=PORT ~/.cache/sunstoneos/venv/bin/python EVIDENCE/host/tools/check-a733-usb-returning.py \
  --fel FEL --image EVIDENCE/probe.bin --ram-input INPUT.bin --evidence RUN_DIRECTORY \
  --expect 'COMPLETE native USB bounded observation' --return-timeout 5 \
  --ddr-evidence DDR_SESSION --platform-survey CURRENT_SURVEY
```

Supply each intervening successful return through `--warm-return`. The runner retains the existing port lock and launch/admission/failure handling. Its observation clock starts before reset64. It admits a new native request only during the first 1.05 seconds and makes one OUT request followed by one IN request, each with a maximum 100 ms timeout. Metadata observation ends by 1.65 seconds and consumes the existing total return deadline. Native absence or late enumeration sends no protocol request and preserves the controlled negative result. A transport failure stops device requests and retains ownership for coordinator recovery. PyUSB handles stay referenced on failure; successful cleanup waits until the old native instance has detached and responsive ROM return is established.

Keep the native log, copied response, input identity and ROM-return result. Functional USB success requires `native_usb.functional_success=true` in the runner's session record. A kernel completion marker alone establishes only the reported bounded outcome and normal monitor return. Stop after the first failure or disappearance; do not automatically reset, reload, reconnect or repeat the test.
