# NPU PLL enable table

The [startup sequencer](../drivers/npu/a733_npu_startup.c) holds the PLL_NPU
method-2 enable table and its predicates over the control word as statics. It
runs the table under the register lock after domain 4 is ON, samples LOCK,
and opens the output gate at the start of its open stage
([npu-platform.md](npu-platform.md)). Physical PLL qualification is outstanding.

The initial NPU operating point is 492 MHz at 800 mV. That voltage belongs to
the NPU domain's DCDC2 supply, which the [power-domain owner](power-domains.md)
verifies. The PLL's internal LDO is a separate control.

## Source basis

The pinned A733 User Manual V1.00 defines the following behaviour. Its central
source-lock identity is `a733-user-manual-1.00`.

| Manual section | Requirement |
| --- | --- |
| 4.1.4.1, pages 25–27 | Disabled enable method 2; enabling or disabling a PLL can disturb other PLLs |
| 4.1.4.2 | Three consecutive lock observations separated by at least 3 microseconds |
| 4.1.4.6, 4.1.6.255 and 4.1.6.264 | Dedicated software gate enabled before selecting No-Auto; effective gate status checked |
| 4.1.6.56–60 | PLL enable, internal LDO, lock control, output gate, factors, modulation and bias fields |
| 4.1.6.76, 114–116 and 120–124 | MBUS, video encode and decode, NPU and DRAM0 consumer selectors and gates |

The board clock source at revision
`2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f` identifies the same PLL register,
24 MHz reference parent and N and divider fields. Its common lock helper waits
for three lock observations, spaces reads by 3 microseconds, limits
observations to 100 and waits another 20 microseconds.
`checks/npu-pll.py` compares the profile and the sequence with these pinned
sources and records their hashes.

The profile uses N=82, M1=1 and P0=4: a 1,968 MHz oscillator divided to
492 MHz. The encoded factor fields are `0x00305100`. The multiplier lies in
the board source's supported 53–105 range and the documented oscillator range.

## Entry state, table and LOCK

The register pass admits this entry state: PLL enable, lock enable, LOCK
and output gate clear. Reserved bit 24 may be clear or set; every other bit
outside the documented fields must be clear. The initial register read captures
bit 24 once, and every later PLL check requires that inherited value. The
LDO and factor fields may hold any value, because the table writes them. The
register pass also requires the PLL_NPU pattern and bias words
(`+0x2a4`–`+0x2b0`) at their reset values, and the video encode, video decode,
NPU and DRAM0 consumers idle or on other sources ([npu-startup.md](npu-startup.md)).

The manual marks bits 26:23 reserved and gives no requirement to clear them.
The pinned kernel's `ccu_nm.c` factor updates and `ccu_gate.c` control updates
preserve bit 24. Its NPU descriptor uses SDM controls in separate pattern
registers; it does not establish a function for control bit 24. Startup therefore
preserves the observed value without treating it as a frequency or SDM setting.
A change at an existing table read, LOCK sample, pre-open check, final check or
local-stop check retains the resources. Bit 24 is never in a writable mask.

The enable table has six entries, each read back:

1. software gate enable, CCU `+0x1920` bit 16;
2. No-Auto, `+0x1920` bit 0;
3. internal LDO, `+0x2a0` bit 30;
4. factors `0x00305100` in the N, P0 and M1 fields;
5. PLL enable, bit 31;
6. lock enable, bit 29.

The LOCK bit (28) is read-only status: the table writes it as zero and the
readback ignores it. The output gate (bit 27) stays closed.

After the register lock is released, the sequencer runs the lock wait that
it shares with the CPU clock and the video decoder (`include/a733_pll_lock.h`).
The wait reads the control word until three consecutive reads show LOCK, at
most 100 times, starting each read at least 3 µs after the previous read
ended, as the kernel helper's `udelay(3)` follows each read. It then waits
20 µs after the third. Only the PLL_NPU token's holder writes this word, so
the sampling needs no lock. The open stage requires the control word
`0xe0305100` with LOCK and the captured bit 24 set as applicable, then opens
the output gate. The final readback requires `0xe8305100` with those same
bits. A LOCK that never appears leaves the PLL enabled with its output closed
and retains every token.

## Adopting a running PLL

Local stop leaves PLL_NPU enabled and locked with its output gate open. A
later image in the same power lifetime therefore finds `0xf8305100`, with the
captured bit 24 as applicable, and the gate controls at `+0x1920` and
`+0x19a0` set. The register pass adopts exactly that state and no other
enabled state. The sequencer then writes neither the PLL nor its gate
controls: it reads the control word once more under the register lock,
samples LOCK three times as above, and leaves the output gate as it is. The
board kernel's PLL enable helper also returns without a write when the
enable, LDO and output gate bits are already set (`ccu_gate.c:171-174`).
Other factors, a closed output gate, a clear gate-control bit or a missing
LOCK refuse before any write; the pinned profile has no rate-change step.

## Two profiles and the rate change

By default the register pass selects the operating point the vendor driver
selects for the SID speed bin ([job overhead design](designs/npu-job-overhead.md#default-operating-point)):
852 MHz at 800 mV for vf0201, vf0400 and vf0500, which includes both boards
(code 4). It refuses every other bin before any write, because that bin's
point needs another DCDC2 voltage or it has no row. `a733_npu_startup_rate`
selects an optional mode instead, before the scheduler: 492 MHz, or 852 MHz.
Both profiles come from the 24 MHz reference, inside the board source's N
range of 53–105, and both run at the inherited 800 mV:

| Rate | N | P0 | Oscillator | Factor field |
| --- | --- | --- | --- | --- |
| 492 MHz (optional mode) | 82 | 4 | 1,968 MHz | `0x00305100` |
| 852 MHz (the vendor point for vf0201, vf0400 and vf0500) | 71 | 2 | 1,704 MHz | `0x00104600` |

The 852 MHz profile is admitted only for a speed bin whose column lists
800 mV for it. The register pass reads SID word `0x0300624c`: the code is
bits 23:16 (`dvfs_bak`) when nonzero, else bits 15:8 (`dvfs_ori`), and
`vf_mapping_table` maps it to a column (`sun60iw2p1-cpu-vf.dtsi` lines 3–17).
The 852 MHz row (lines 2069–2091) lists 800 mV in both oscillator columns of
vf0200, vf0201, vf0400, vf0500 and vf0501, codes `0x02`, `0x12`, `0x04`,
`0x05` and `0x15`. Every other code refuses before any write: an
unprogrammed 0, a code without a column in the NPU table (vf0100, vf0101,
vf0300, vf0600), and vf0401, which lists 820 mV. Port 3-3 reads `0x496`,
code 4, column vf0400.

A PLL that an earlier start left locked at the other profile, with its
output gate open and its gate controls set, is retuned in the order of the
board kernel's `ccu_nm_set_rate` with `CONFIG_AW_STANDARD_CCU`, which both
shipped board kernel configurations set (`ccu_nm.c` lines 250–344):

1. output gate off (line 264);
2. the N and output-divider factors in one write (314);
3. lock enable cleared, then set (329–334);
4. LOCK, three consecutive observations (339);
5. output gate on, at the start of the open stage (342).

Each write is read back. The rate changes only while the NPU module gate is
closed and the NPU's resets are asserted, which the register pass requires.

## Enabling the PLL while DRAM runs

The sequencer enables PLL_NPU from a DRAM-resident FreeRTOS task. The board's
Linux runtime does the same at the pinned revision:
`vip_drv_device_platform.c` calls `clk_set_rate(pclk)` and later
`clk_prepare_enable(mclk)`, and the device tree binds that parent to PLL_NPU.
The CCU descriptor uses `CLK_SET_RATE_UNGATE`, and the pinned
[clock core](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/drivers/clk/clk.c#L2278)
enables such a clock before changing its rate. The CCU gate helper handles a
disabled PLL as output off, LDO on, PLL on, lock detector on, wait for lock,
then output on.

That precedent does not establish the board's state or physical safety. The
manual warns that enabling a PLL can disturb other PLLs, and independent
selectors do not remove that analogue risk. The session record's clock
evidence therefore binds these conditions: independent DRAM and PHY sources,
exclusive PLL_NPU consumers and writers, no autonomous parent-selector
writers, and supervised acceptance of the cross-PLL disturbance risk. Each
session is a single supervised attempt that ends with whole-board power
removal. Unlike the Linux helper, which only warns on a lock failure, the
sequencer retains the failure without opening the output.

## Native write rules

The [native adapter](../drivers/npu/a733_npu_platform_native.c) admits writes
to PLL_NPU control at `+0x2a0` and gate control at `+0x1920` only under these
rules:

- LOCK is never written;
- the PLL and its LDO are never disabled;
- the factors change only to `0x00305100` or `0x00104600`, and only with the
  output closed;
- the output opens only with the PLL, LDO, lock enable and LOCK set;
- the two gate-control bits never clear.

The rules guard against a wrong table entry;
the clock token remains the authority.

## Checks

`checks/npu-startup-host.c` runs the table through the sequencer, including a
PLL that never locks; its `--second-start` cases adopt the stopped PLL with
bit 24 clear and set and refuse the other enabled states. `checks/npu-stop-native.py` exercises the adapter's PLL
rules in the linked images, and `checks/npu-application-clock-reference.py`
shows that an admitted reference case requests domain 4 ON and makes no
device write before the domain is ON.

The sanitizer check `checks/npu-startup-host.c --inherited-pll` covers the
observed disabled word `0x41104500` and the equal-LDO word `0x40104500`.
Both use 12 writes, one of them to the routing word, 25 counter reads and
four register-lock acquisitions.
The captured field adds 16 bytes to the native startup structure, including
alignment padding, and 60 bytes to the compiled startup functions.
It also covers both drift directions at six existing validation boundaries and
refuses entry with reserved bits 23, 25 or 26 set. The original 15-case baseline
has identical counts. These are modeled register results, not physical PLL
qualification. Evidence is in
`~/.cache/sunstoneos/evidence/npu-pll-inherited-20261024/`.
