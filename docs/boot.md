# A733 recovery and first code execution

The original board answered FEL requests with SoC ID `0x1903`, the A733 identifier
in the recovery tool. Its successful identity response on 2026-10-15 reported
protocol version 1 and scratchpad address `0x000ee800`. USB bus/address was
`003:029`; that address supplies no current access authority.

The results below describe the initial boot probes and their limits at execution.
The [current handoff](handoff.md) records subsequent CPU results and whether board
access is available. Neither current board has populated eMMC or UFS; both have
microSD cards whose contents must be preserved. Boot-security configuration
remains unconfirmed. The original board's DDR initialization reported 12 GiB;
full capacity testing remains outstanding.

## Standalone microSD boot remains unimplemented

The pinned board packaging recipe places boot0 at byte 8,192 and a TOC1
package at byte 16,793,600 in a zero-filled 20 MiB file. Its package contains
U-Boot, monitor and SCP firmware. This describes the published container;
it does not establish a SunstoneOS boot chain or authorize writing either card.
The source assessment and exact reference hashes are preserved under
`~/.cache/sunstoneos/evidence/microsd-boot-package-20261020/`.

The current SunstoneOS entry requires EL3 and a sealed launch contract supplied
by the RAM loader. Packing that ELF as U-Boot does not establish either
precondition. The pinned monitor's header and inspected code use `0x48000000`,
which is also the start of the current extended device pool. Retaining that
monitor would require an explicit memory reservation and a compatible CPU
entry contract. SCP activity also needs an ownership decision before the OS
can claim exclusive control of shared clocks and power.

The selected standalone design uses a SunstoneOS monitor payload and omits
U-Boot, the published monitor and SCP firmware. A focused instruction check
executes the pinned boot0 package parser and its final monitor handoff with
that component list. The parser loads the monitor and the handoff requests
an AArch64 warm reset to its address. It accepts zero U-Boot and OP-TEE
entry pointers. The payload itself does not execute in this check.

Boot0 takes the monitor load address from payload offset `0x2c`; the TOC1
item's `run_addr` does not select it. After copying the payload, boot0 replaces
that word with the DRAM size in MiB. It also writes eight boot-header bytes
at `0x24`, 128 DRAM-parameter bytes at `0x30`, U-Boot and OP-TEE pointers
at `0x10` and `0x18`, and sixteen identity bytes at `0xb8`.
The entry branch must skip this mutable header. The selected payload reservation
is `0x47000000..0x47400000`, outside boot0's package staging at `0x42e00000`,
the existing application/monitor allocations, and the extended device pool.
Container capacity further limits the payload length.

The check uses a synthetic package already in RAM. Storage loading, checksum
admission, DRAM training and cache cleanup are outside its executed interval.
It models the identity source and reset-mode control; register writes do not
establish a physical reset or entry at EL3. Omitting an SCP item also does not
prove that remote processors remain stopped throughout cold boot.
The reusable check is [boot0-monitor-handoff.py](../checks/boot0-monitor-handoff.py);
its result is `microsd-boot-package-20261020/boot0-monitor-handoff.json`
under the evidence directory.

The pinned board packer produces a sole-monitor TOC1 package from synthetic
bytes. Its output matches the assessed item geometry, padding and additive
checksum. This establishes host container generation only. The
[ordinary-file packager guide](microsd-boot-package.md) documents the implemented
wrapper, input constraints and preservation checks. The packer runs
on the development host; its source equivalence and individual redistribution
terms remain unresolved. Evidence is in
`microsd-boot-package-20261020/actual-packer/`.

A cold-entry implementation must still establish EL3 state, remote-processor
exclusion, resource ownership, application admission and recovery. No standalone
SunstoneOS payload, storage write or physical boot qualification has occurred.

## SRAM probe result

The 60-byte probe in [checks/a733-sram.S](../checks/a733-sram.S) executes in
AArch32 at `0x52000`, records CPU registers at `0x52200`, and returns to FEL.
The recovery tool reserves `0x71000` for its instruction-cache workaround. The session
saved 64 bytes at each address before loading code and restored those bytes after
reading the result. Readback matched every saved region.

| Register or value | Physical result | Meaning |
| --- | --- | --- |
| Probe signature | `0x53534f53` | The probe reached its result store |
| CPSR | `0x60000153` | AArch32 supervisor mode |
| MIDR | `0x412fd050` | Arm Cortex-A55, revision r2p0 |
| MPIDR | `0x81000000` | Boot processing element affinity value |
| SCTLR | `0x00c51838` | MMU and data cache disabled; instruction cache enabled |

The tool's internal write function disables instruction caching, but its public
`write` command bypasses that function. The probe confirms that instruction
caching remained enabled. The RAM loader must explicitly invalidate instructions
before replacing code at an address that has already executed. The session
issued no persistent-storage operations or device reset.

The initial combined command stopped after execution because the tool consumed
the next command name as an extra argument to `exe`. This was a host parser
failure; no USB transport error was reported. A separate transaction read the
valid result and restored SRAM. The repository includes a one-line parser fix.

Raw files and register results are under
`~/.cache/sunstoneos/evidence/a733-sram`. The executed probe SHA-256 is
`d36ab47822e09c041810625c497bae3a57aeeb37ed6f7af63ca2034d3160fef7`.
The recovery executable SHA-256 is
`0003ca317ce471ef8911cfb89e5c8322e28164f5c968d5e474f0d5e3befee491`.

## DDR initialization and RAM transfer

The wrapper in [boot/a733/ddr-entry.S](../boot/a733/ddr-entry.S) calls selected
functions from the pinned board boot0 binary. It runs board power/clock setup,
copies the selected DRAM parameters, and invokes DDR training. It replaces UART
character output with a bounded SRAM log. It does not enter the normal boot
function or its storage-loading paths.

The binary occupies `0x47000` through `0x82fff`; its BSS ends at `0x83c28`.
The wrapper executes at `0x85000`, with a stack ending at `0x8d000`. It swaps the
12 KiB overlapping live FEL code with a shadow at `0x8e000`, then restores that
code before returning. A separate entry disables and invalidates instruction
caching before the loader replaces previously executable SRAM.

On 2026-10-15, this path returned to FEL with stage 5, board setup result 0, and
DDR size 12,288 MiB. Firmware identified LPDDR5 and reported successful training
at 2040, 1165, 816, and 408 MHz. Its simple memory check passed. Host transfers
then wrote distinct 4 KiB patterns at `0x41000000` and `0x42000000`; both matched
after both writes. These observations establish usable DDR at those addresses.
They do not qualify the reported capacity, stability, or every frequency.

Raw evidence is under `~/.cache/sunstoneos/evidence/ddr-20261015-223537`.
Its manifest identifies each payload and the recovery executable. The wrapper
SHA-256 is `a401ce2b7f0581bf1c8712cdc30d8a8d28a00b5b934e577b39ddeef4c1dafb8d`.
No persistent-storage writes were issued.

Build the reviewed payloads with `python3 tools/prepare-ddr.py`. The script binds
its function addresses to the reviewed boot0 hash and checks the header, BSS,
patch bytes, and payload bounds. These checks remain active under Python `-O`.
The resulting files are RAM payloads, not standalone flashable boot images.

The host loader requires the patched recovery tool:

```sh
python3 tools/run-ddr.py --fel ~/.cache/sunstoneos/build/sunxi-a733/sunxi-fel
```

Run the loader only when DDR initialization is needed. It records SRAM backups,
checks complete uploads before execution, and stops at the first failure.
The separate wrapper check uses Unicorn 2.1.4:

```sh
~/.cache/sunstoneos/venv/bin/python checks/ddr-wrapper.py ~/.cache/sunstoneos/build/a733-ddr
```

That check passed success and failure returns, FEL register/code preservation,
parameter selection, and the logging trampoline. It substitutes the blob calls
and therefore does not simulate DDR, clocks, or power hardware.

## AArch64 entry and return to FEL

The 136-byte probe in [checks/a733-aarch64.S](../checks/a733-aarch64.S) executes
at `0x41000000` and writes eight 64-bit values at `0x42000000`. It then sets the
reset vector to the ROM's forced-FEL entry at `0x20` and requests an AArch32 warm
reset. Before execution, five ROM instruction words matched the reference dump.
The board re-enumerated in FEL, and the saved DRAM result remained readable.

| Register or value | Physical result | Meaning |
| --- | --- | --- |
| Signature | `0x41363450` | The AArch64 probe completed its stores |
| CurrentEL | `0xc` | Execution at EL3 |
| MIDR_EL1 | `0x412fd050` | Cortex-A55 r2p0 |
| MPIDR_EL1 | `0x81000000` | Boot CPU affinity |
| CNTFRQ_EL0 | `0` | Software must initialize the frequency declaration |
| CNTPCT_EL0 | `0x18510c0faa` | Physical counter value at capture |
| RVBAR_EL3 | `0x41000000` | Probe reset entry |
| SCTLR_EL3 | `0x00c50838` | MMU and caches disabled |

The counter value alone does not establish its frequency. The EL1 handoff below configures access and timer metadata.
The [physical kernel check](a733-kernel.md) adds interrupt-controller setup and FreeRTOS startup.

Raw evidence is under `~/.cache/sunstoneos/evidence/a733-aarch64`. Probe SHA-256:
`2726fbc07cc6882749b28a13fa0462d04567fbe0a75ce8283a7cf0bf76093e49`.
DDR and AArch64 execution used recovery executable SHA-256
`148109fa5fe7ed8f89281f83f7d440b978a7c85af2813217e5dc7da13b8ba37e`.

USB re-enumeration replaces the device node and its per-node ACL. Install the
[session access rule](../tools/README.md) before further intentional reconnects.
The rule supplies permissions on the selected physical port until host reboot.

Issue [1](https://github.com/pedapudi/sunstoneos/issues/1) tracks recovery and
storage preservation. Issue [2](https://github.com/pedapudi/sunstoneos/issues/2)
tracks the physical kernel milestone and remaining CPU platform work.

## EL1 handoff and automatic reconnect access

The probe in [checks/a733-el1.S](../checks/a733-el1.S) configures nonsecure
AArch64 EL1 with MMU and caches disabled. It declares the device tree's 24 MHz
counter frequency, permits EL1 counter/timer access, and installs exception
vectors. EL1 records its state and uses a secure monitor call (SMC) to return
to EL3. The handler requests the tested forced-FEL warm reset. Unexpected
synchronous exceptions also record their state and enter the recovery path.

The probe passed once in QEMU with its semihosting exit and once on the physical
board. Hardware returned CurrentEL `4`, CNTFRQ_EL0 `24000000`, stage `4`, and
status `0`. No EL1 exception was recorded. The counter advanced between the
EL1 capture and EL3 return. Frequency calibration and timer interrupts remain
untested.

During this same physical check, the USB node changed from `003:030` to `003:031`.
The installed session rule granted access to the new node. The host identified
A733 and read the successful result without another user command. The runner
checks the reconnect rule before reset and polls only host metadata while
waiting for the new node and its permissions. It never retries a failed USB
protocol request or resets again to repair permissions.

Raw evidence is under `~/.cache/sunstoneos/evidence/el1-20261015-225620`.
The 6144-byte physical image SHA-256 is
`b3bf6aeaa9d75b2c0b3ec2d3c8d46c33d84dd0d603a7c31d7dce8ef95eda49dc`.
Build from the worktree with Clang and the locally extracted LLD:

```sh
cache="$HOME/.cache/sunstoneos"
mkdir -p "$cache/build/a733-el1"
clang --target=aarch64-none-elf -c checks/a733-el1.S -o "$cache/build/a733-el1/probe.o"
"$cache/host/usr/lib/llvm-21/bin/ld.lld" -Ttext=0x41000000 -e _start \
  "$cache/build/a733-el1/probe.o" -o "$cache/build/a733-el1/probe.elf"
/usr/lib/llvm-21/bin/llvm-objcopy -O binary \
  "$cache/build/a733-el1/probe.elf" "$cache/build/a733-el1/probe.bin"
python3 tools/check-a733-el1.py --fel "$cache/build/sunxi-a733/sunxi-fel"
```

The runner requires initialized DDR containing a prior AArch64 probe signature.
For emulator validation, assemble with `-DQEMU_CHECK`, link at the same address,
and load that ELF with QEMU's loader device. Use `virt,secure=on,virtualization=on,gic-version=3`,
one Cortex-A55, 128 MiB RAM, and semihosting. Only the final recovery operation
changes: QEMU exits through semihosting, while the board resets into ROM.
