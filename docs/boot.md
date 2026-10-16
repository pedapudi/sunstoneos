# A733 recovery and first code execution

The attached device answers FEL requests with SoC ID `0x1903`, the A733 identifier
in the proposed recovery-tool support. The successful identity response on
2026-10-15 reported protocol version 1 and scratchpad address `0x000ee800`.
USB bus/address was `003:029`; rediscover the device before each session.

Board revision, RAM capacity, populated storage, and boot-security configuration
remain unconfirmed. FEL identity alone cannot establish these properties.

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

## DDR initialization remains the next physical dependency

The board-specific boot0 binary is pinned in the source manifest. Its header
declares load and entry address `0x47000` and length `0x3c000` (240 KiB).
Its parameters identify LPDDR5, and its strings include DDR training and storage
boot paths. These observations do not prove the attached board's RAM configuration.

The original A733 recovery patch reserves only 180 KiB and places its scratch
and return code inside the larger boot0 image range. Loading boot0 with that
layout would overwrite the recovery code. Review the larger SRAM layout and
the boot0 storage/return behavior before execution. A DDR-only entry that returns
to FEL is preferable to allowing unknown storage boot paths to run.

Once DDR initialization and recovery are established, inspect the monitor's
AArch64 handoff and provide the FreeRTOS EL1 entry contract. Preserve existing
storage before any persistent installation. Issue
[1](https://github.com/pedapudi/sunstoneos/issues/1) tracks these requirements.
