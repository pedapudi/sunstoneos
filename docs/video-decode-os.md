# Video decoder in the OS image

The VE decoder at `0x01c0e000` (JPEG, H.264 and HEVC) is an OS-image service. An application declares the `video-decode` device in its manifest, the session grants it as `SS_DEVICE_VIDEO_DECODE`, and the application calls the class interface in include/sunstone_video_decode.h. One owner task (apps/os/video_decode.c, priority 3, pinned to CPU 4) makes every decoder, CCU and GIC access through the decoder platform binding (drivers/video/a733_video_platform.c). The service and the binding are an object library that an image links only when its application declares `video-decode` (cmake/sunstone-image.cmake), so other images keep their code placement. EL3 admission refuses a grant that the manifest does not declare, so a granted device always has its service. The service writes no PCK600 or PMIC register: the shared power-domain owner (drivers/power/a733_power_domains.c) powers domain 2 for it.

Status: open and close are implemented, host-tested by the cold-start model (`checks/video-cold.py`), and passed board stage 1 on port 3-1 (run `1794714415147688285-6b3ec3f5`, [Board stages](#board-stages)). After a close the decoder stays closed for the rest of the board's power lifetime; a second open is refused. No decode runs through the service yet; INTID 81 stays disabled and its handler is empty.

## Vendor sequence and the native diff

The reference is the vendor VE driver `cedar_ve.c` and the clock framework (`ccu_gate.c`, `ccu_common.c`, `ccu-sun60iw2.c`) in linux-orangepi 2ac08e8c, the PCK600 domain driver `pck600_domains.c`, the device tree `sun60iw2p1.dtsi` and the operating-point table `sun60iw2p1-cpu-vf.dtsi`. The [PLL design](designs/video-pll.md) and the [platform binding](video-platform.md) derive each step; this table gives the order and the difference.

| Vendor step | Reference | OS image |
| --- | --- | --- |
| Power domain 2 on before any decoder clock or reset changes | genpd attach at probe; delay row and ON command, `pck600_domains.c` 126-142 and 547-569 | The start entry registers a domain-2 row before the scheduler starts. Open requests ON once; the owner writes the delay row and the ON command, or adopts a domain already ON with no write. The binding writes no PCK600 word. |
| VDD-VE on DCDC2 at 800 mV | board schematic v1.2, sheets 2 and 6 | The owner task holds the DCDC2 rail token and requires the owner's DCDC2-verified bit. No PMIC write. |
| Critical MBUS clock gates `0x5c0` bit 0, `0x5e0` bit 14, `0x5e4` bit 18 | `CLK_IS_CRITICAL`, `ccu-sun60iw2.c` 622, 663, 688-692; enable `ccu_gate_helper_enable`, `ccu_gate.c` 213-240 | `0x5c0` and `0x5e0` are read and must be set; `0x5e4` is opened once with the key value `0x40302` ([video-platform.md](video-platform.md#cold-startup-identifies-a-fixed-rom-profile-before-power-on), "The MBUS clock gate"). |
| Reset release (`0xa8c` bit 18), bus gate (`0xa8c` bit 2) | `enable_cedar_hw_clk_reset`, `cedar_ve.c` 368-386; `enable_cedar_hw_clk_module`, 416-472 | Same order, each with readback, after the three critical gates read set. |
| Module parent PLL_VE0 (mux 1), divider 1, gate closed | `clk_set_parent` at probe, `cedar_ve.c` 1789; parent list `ccu-sun60iw2.c` 965-981 | `0xa88` selector and divider `0x01000000`, gate closed. |
| PLL_VE0 at 624 MHz: LDO, factors, enable, lock enable, LOCK wait, output gate | `setup_ve_freq`, `cedar_ve.c` 351-366; `ccu_gate.c` 157-211; lock wait `ccu_common.c` 47-64 | Factors `0x00204d00` (N=78, P0=3) written before the enable, as the manual's enable method 2 orders them; one lock wait with the kernel's bounds (three reads in a row with LOCK, at most 100 reads, 3 µs apart, then 20 µs); output gate; readback `0xf9204d00`. |
| Module gate (`0xa88` bit 31) last | `enable_cedar_hw_clk_module`, `cedar_ve.c` 416-472 | Same; `0xa88` then reads `0x81000000`. |
| Cold VCU pulse at `0x01c0e804`, bits 0 and 4 set then cleared | `set_vcu_en_regmode_to1_to0`, `cedar_ve.c` 388-405 (`CONFIG_ARCH_SUN60IW2`): read, write with the bits set, read, write with them clear; no readback check | Same, with no readback check: no source documents how the pulse bits read back (the manual has no VE register section). The two words read are recorded. Then the identity words at `+0xf0`, `+0xe0` and `+0xe4`. |
| Close: module gate, bus gate, reset, then PLL_VE0 output gate, lock enable, enable and LDO | `disable_cedar_hw_clk_module`, `disable_cedar_hw_clk_reset`; `ccu_pll_gate_helper_disable`, `ccu_gate.c` 40-69 | Same order in one register-lock hold ([video-platform.md, Close](video-platform.md#close)). |

The open adds checks that the vendor driver does not make, each read before the first write: reset asserted and bus and module gates closed, `0x1b10 & 0x30001 == 0x20000` (CM_VE_DEC_CFG with its module mode disabled and its status field at POWER_ON), the fabric and parent clocks, the 15 boot ROM words, and INTID 81 neither enabled, pending nor active. A refusal at any of these writes nothing. The service waits for the row's answer on the row's wake, under two bounds: until supply verification ends, the owner's supply budget (`A733_POWER_SUPPLY_SETTLE_US` plus `A733_POWER_SUPPLY_BUDGET_US` plus one tick, anchored at the PMIC startup deadline or the first wait, whichever is later), and once DCDC2 is verified, the 10,000 µs PCK600 bound plus two ticks; the open's own deadline caps both. A row that the owner has not answered by its bound writes nothing either: the decoder requests the row only after the entry hold and writes only after the ON answer, so the service records step `VD_STEP_ROW_TIMEOUT` and ends CLOSED with no close. It records the same step when the open's deadline passes first and the binding refuses the open with the row still pending. The binding then drops the refused open's deadline (`a733_video_decoder_cancel`), and an open refused only at its own deadline is not left failed, so a later open resumes at the outstanding row request. A failure after the first write is handled as the next section describes.

## Faults after the first write

The service follows the rule in [the failure-return audit](designs/failure-return-audit.md#a-faulted-device-service-closes-before-it-reports): a faulted device service closes before it reports.

1. A failure after the first write puts the service in FAULT. The open returns -1, and `ss_video_decode_fault` gives the step and value. The service keeps its tokens and gates and starts nothing new.
2. The application's close, or the session quiesce if it comes first, runs the binding's close once with `faulted` set ([video-platform.md, Close](video-platform.md#close)).
3. When every step reads back, the service is CLOSED and stays closed for the power lifetime. The close answers -1, the fault stays in `ss_video_decode_fault`, and the quiesce reports DONE. The session therefore ends with a normal return and its result record.
4. Two cases make the service stay FAULT, and the quiesce then reports fault site `0x941`, with the failing step in bits 31:24 and its value in bits 23:0:
   - a close step whose readback differs;
   - a binding failure whose effect is unknown, which the close does not touch.

The record names the word a refused comparison judged and why the unit failed ([video-platform.md](video-platform.md#cold-startup-identifies-a-fixed-rom-profile-before-power-on), "Failures").

[checks/os-vdec-fault-return.py](../checks/os-vdec-fault-return.py) executes the image's own quiesce registry, owner task and binding, starting from the state that the stage-1 run `1794712072331160946-bd447f26` left when hold B refused:
- the quiesce or an application close closes the decoder, and the registry reaches DONE within the 3 s bound with no fault note;
- a close step that does not hold, or an uncertain failure, stays FAULT at site `0x941`.

The image of that run fails the first two cases.

## The stage-1 record

The class interface carries no register dump. An image built with `SUNSTONE_VIDEO_DECODE_MEASURE` (the vdec-check image) also keeps the stage-1 record, `struct vdec_check_record` in apps/vdec-check/record.h, which the application copies with `vdec_check_record` after each call (560 bytes):

- 20 platform words read under the register lock at open before any write, after the cold start, and after the close: the PLL reference, the RTC oscillator word, the AHB, NSI and MBUS clocks, PLL_VE0 and its pattern words, the PLL gate configuration and status, `0x1b10`, the module word, the reset and bus gate word, the three critical gates, the PCK600 bus clock (R_CCU `0x1ac`) and the GIC enable, pending and active words for INTID 81;
- the binding's cold record (`struct a733_video_cold_record`): the critical gates at entry and before the reset release, the `0x5e4` word written and read back, the phase times and the close's hold ([video-platform.md](video-platform.md#register-lock-holds-bound-band-2-waits)), the number of PLL lock reads, and the two `0x804` words the VCU pulse read before its set and clear writes;
- after a fault: the step and value, the unit's failed, mutex-held, uncertain and late flags, the address and word of the binding's latest access before the close, and whether the fault close completed (the closed words are then the words after it);
- the identity words and the admitted compound version;
- the domain-2 row's answer, the power authority word at that answer, and whether the owner measured transitions back to back;
- the decoder task's row request and answer times, the power-domain owner's first step and the step that published DCDC2 verified, all on the owner's clock, and the system counter at the open request, the started decoder and the close;
- domain 2's transition record as the power-domain owner published it with the row's latest answer (`transition` in `struct a733_power_domain_row`).

apps/vdec-check/host.py decodes the 592-byte result of the vdec-check application, which is the record behind a 32-byte header, and lists every departure from the stage-1 prediction.

## Board stages

Each stage is one run on port 3-1 of the image `sunstone-a733-os-vdec-check` (`-DSUNSTONE_APPLICATION=vdec-check`).

1. **Power, PLL and identity.** Open, then close (also after a failed open, so that a fault is closed and the record returned); no decode. The image calls `a733_power_domains_measure_transitions()`, so the power-domain owner reads status back to back from each PCK600 command to its target or the 10,000 µs bound. That spin runs in the PMIC service task, priority 3 on core 0. For up to 10 ms it holds off band-1 tasks on core 0, core 0's WFI, other PMIC requests and other domains' row answers; higher-priority tasks and interrupts still preempt it. Each command overwrites its domain's record, and the owner publishes the record with each answer to the domain-2 row. A run therefore yields at most one sample: one when domain 2 reads OFF at entry, none when it is adopted ON. Domain 2 stays ON across the close, so at most one sample comes per board power cycle; domain 5 stands in for it (docs/power-domains.md, "A transition record sizes the 50 µs spin").
2. **JPEG.** One baseline JPEG decode compared with the pinned vendor-library reference; INTID 81 masked again at the close.
3. **H.264.** One frame compared with the same reference.

The stage-1 prediction, from the port 3-3 FEL dump and the host model:

| Record field | Predicted |
| --- | --- |
| Entry `0x5c0`, `0x5e0`, `0x5e4` | bit 0 set, bit 14 set, `0x4` |
| `0x5e4` written and read back | `0x40306`, `0x40306` (`0x40004` if the key bits read zero) |
| PLL_VE0 at entry, started, closed | `0x41305a00`, `0xf9204d00`, `0x01204d00` |
| Module word at entry, started, closed | `0`, `0x81000000`, `0x01000000` |
| Reset and bus gate (`0xa8c`) at entry, started, closed | `0`, `0x40004`, `0` |
| PLL lock reads | 3 to 100 |
| Identity | one of `0x0003332100021321`, `0x0003331000021320`, `0x0003052000012011`, with `+0xf0` reading `0` or `0x16670040` |
| Domain 2 | row ON; either adopted at policy and status 8, or one command from 0 to 8 measured within 10,000 µs |
| `0x1b10` at entry and after the close | `0x20000`: module mode disabled, status POWER_ON (recorded after the close, not judged) |
| `0x5e4` bits 1, 8 and 9 (encoder, CSI and ISP MBUS clock gates) | set by the open and still set after the close |

Run `1794714415147688285-6b3ec3f5` on port 3-1 matched every row and returned normally in 21.8 s:

- The entry words were those of run `1794709569136470404-9a7b6a30`, so the watchdog reset that ended the hold-B failure restored the decoder's CCU words.
- `0x5e4` was written and read back as `0x40306`; the lock wait took 3 reads.
- The identity was `0x0003331000021320` with `+0xf0` reading `0`.
- Domain 2 was adopted ON at policy and status 8, with no command and no sample.
- `0x804` read `0x5555aaaa` both before and after the write that sets bits 0 and 4: the pulse bits read zero. The exact readback in the image of run `1794712072331160946-bd447f26` therefore refused hold B.

**Image identity.** The image that run ran, `7d63776e…5e4c`, is what commit 6fef74d18 builds, apart from its build id:

- A rebuild of 6fef74d18 from a separate checkout, with a source path and a build directory of the same lengths as the original's, differs from it in 48 bytes, all outside code. 32 are `a733_smp_build_id` in `.rodata` (`0x41200d88`); 16 are in `.debug_line`, which is not loaded. Every other loaded byte is equal.
- The build id (cmake/a733-board.cmake) hashes the digests of `board_inputs` in list order. In the rebuild that list held the same files in a different order, because its order follows the checkout's absolute path. The build id therefore changes with where the tree is checked out, a reproducibility defect that this branch does not fix. The path-map order in cmake/aarch64-clang.cmake, sorted by root length, also changes with path lengths, which is why the rebuild used equal lengths.
- The record's `git_revision`, `4f4d44e73`, is not the image's source. tools/board.py sets it from `git rev-parse HEAD` in the checkout that serves the board queue, the bringup worktree, which was at 4f4d44e73 when the run started.
