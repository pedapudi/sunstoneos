# Start the NPU through one sequencer on the power-domain owner

The [startup sequencer](../drivers/npu/a733_npu_startup.c) powers, clocks and
releases the A733 NPU from reset, then lets the [NPU service](npu-service.md)
activate. One privileged bootstrap task calls one step function once per tick
until it returns READY. The sequencer borrows the image's providers: the
counter clock, the clock registry, the register lock, the image's one GPU/PCK
resource manager, the rail ledger with its power-authority word, and the
[power-domain owner](power-domains.md). It constructs none of them.

Status: implemented, checked in a host model, in two-core QEMU and in the
linked images under Unicorn. No board has run it. The design and its
budgets are in [designs/npu-board.md](designs/npu-board.md); the measured
counts are in [designs/npu-board-measurements.md](designs/npu-board-measurements.md).
The initial operating point is 492 MHz at 800 mV.

## Construction before the scheduler

`a733_npu_startup_init` runs at EL1 before the scheduler starts. It copies the
configuration and the provider view, and registers one domain-4 row with the
power-domain owner. The configuration holds:

- the fabric record: the AHB, NSI and MBUS selector, divisor and enable words
  with their exact rates ([platform tables](npu-platform.md));
- the owner identity used for the clock, rail and row requests;
- the session deadline in counter microseconds;
- the image's PCK client number for the NPU.

Initialization refuses a fabric record outside the supported SYS24 and PERI0
profiles, a provider that is missing or uninitialized, and storage that
overlaps the configuration or the provider view.

The optional operations structure supplies register, time and lock callbacks
for host models. With `NULL`, the sequencer uses the native ones: the NPU
allowlist adapter ([`a733_npu_platform_native.c`](../drivers/npu/a733_npu_platform_native.c)),
the counter clock, `CNTPCT_EL0` for LOCK spacing and the shared register lock.
`a733_npu_startup_bind_task` then names the one task that may call the step,
stop and activation functions.

## One step function with counter deadlines

Each call to `a733_npu_startup_step` checks the calling task, EL1 and the
scheduler state, takes one counter sample, and runs stages until one has to
wait. A waiting stage returns PENDING; every wait has a counter deadline.

1. **Tokens.** One clock-registry token: exclusive PLL_NPU and NPU-local
   scopes, shared fabric, reference and oscillator, and shared PERI0 when a
   fabric clock selects it. One shared DCDC2 rail token from the ledger. One
   stable OSC and PCK lease from the GPU/PCK manager, with its parent lease
   when PERI0 is selected. A busy provider returns PENDING.
2. **Supply.** One acquire load of the power-authority word. While the owner's
   supply-done bit is clear the step returns PENDING. The deadline is the later
   of the token step and the PMIC provider's startup deadline, plus the owner's
   5,160 µs settle, its 100 ms verification budget and one tick. A PMIC
   transfer or domain-4 fault bit, a done bit without DCDC2 verified, or an
   expired deadline refuses.
3. **Register pass and PLL enable.** Under one register-lock acquisition: the
   owner's domain-4 check, which reads the PCK600 words once and returns the
   domain state (startup requires OFF), the register pass below, and the
   PLL_NPU method-2 enable table ([PLL table](npu-pll.md)). The lock is released, and LOCK is
   read without it: at most 100 observations 3 µs apart until three in a row
   read locked, then 20 µs. The worst case is about 320 µs, so the spin does
   not yield.
4. **Local open.** Under a second acquisition, PLL_NPU locked with its gate
   controls set, then the local open table with a readback of each write.
5. **Domain 4.** `a733_power_domain_request(row, 1)`. The power service
   answers the row; the step returns PENDING while it is pending. The deadline
   is the request plus `A733_POWER_PCK_BOUND_US` (10 ms) plus two ticks of
   service latency.
6. **Settle.** When the row reads ON, a counter sample sets the settle end
   2,000 µs later; the step returns PENDING until a later sample reaches it.
   No pinned source gives this settle.
7. **Final readback.** Under a third acquisition, domain-4 policy and status
   ON, the bus and reset word, the module word and the PLL_NPU control word.
   The step then returns READY.

Steps 4 and 5 keep the clock-before-power order of the pinned VIPLite
`vip_drv_device_platform.c` (lines 419–428). The sequencer writes no PCK600
register and issues no PMIC transaction; the power-domain owner does both.

In the host model with the PMIC at 400 kHz, the sequencer reaches READY 11
ticks after the scheduler starts, in 12 calls of which 4 advance a stage. It
makes 57 register reads besides the LOCK samples (52 by the sequencer and 5
by the owner's domain check), 12 writes and 3 lock
acquisitions.

## The register pass refuses before any write

The pass reads each word and compares it under a mask with a constant:

- the oscillator and reference PLL, through the shared 24 MHz reference
  decoder ([clock-leases.md](clock-leases.md#detected-oscillator-and-normalized-reference));
- the AHB, NSI and MBUS words against the fabric record, with no update
  pending;
- when a fabric clock selects PERI0: PERI0 enabled, locked and unmodulated,
  and each selected output's No-Auto route, software enable and effective
  gate;
- the other PLL_NPU consumers: video encode and decode module gates closed and
  their selectors in range, their bus gates closed and resets asserted;
- the NPU module gate closed, its bus gate closed, core, AXI and AHB resets
  asserted and SRAM reset released;
- DRAM0 enabled from DDRPLL with no update pending and its PHY on the
  separate PHY PLL; video and NPU clock-management modes disabled;
- PLL_NPU disabled, with its pattern and bias words (`+0x2a4`–`+0x2b0`) at
  their reset values.

The domain-4 policy and status come from the owner's check, which precedes
the pass. The pass takes 20 reads, or 25 when PERI0 is selected.

## Failure keeps everything after the first write

A refusal before the first NPU register write releases the PCK lease, the rail
token and the clock token, and returns RETAINED with nothing written. Any
failure after that write returns RETAINED and keeps the tokens, the row with
its request, and the register lock if a table stopped midway. A LOCK that
never appears leaves PLL_NPU enabled with its output gate closed. Further
calls return RETAINED without device access. Recovery is supervised
whole-board power removal.

## Local stop after CLOSE

`a733_npu_startup_stop` runs on the bound task after the controller's CLOSE
succeeds. Under one acquisition it checks the state that startup left, then
applies the local stop table: module gate off, bus gate off, then the AHB, AXI
and core resets asserted, in the order of `npu_clk_uninit` in the pinned
VIPLite platform. Each write is read back. PLL_NPU, the SRAM reset, domain 4
and every token stay held.

## Service activation

The convolution, session and runtime bindings construct the deferred service
with an activation callback that runs on the bootstrap task. The callback
compares the service's copy of the backend operations, MMU image and command
catalogue once, then calls `a733_npu_startup_prepared`. That function returns
0 before READY, 1 when READY with the counter, registry and register lock
healthy, and -1 after any failure or from another task. Activation therefore
publishes the service only after the sequencer reached READY. No startup
call hashes models or command streams.

Integration follows this order:

1. Construct the counter, registry, locks, GPU/PCK manager, rail ledger, PMIC
   provider and power-domain owner before the scheduler.
2. Initialize the startup, create the bootstrap task at EL1 and bind it.
3. Bind the convolution, session or runtime to the startup and its clients.
4. After the scheduler starts, the bootstrap task calls the binding's startup
   poll once per tick until READY, then `npu_service_activate`.

## Checks

- [`checks/npu-startup-host.c`](../checks/npu-startup-host.c) runs the
  sequencer with the actual PMIC provider, rail ledger, clock registry,
  GPU/PCK manager and power-domain owner over a register model. It checks
  boot to READY with its counts, local stop, three refusals before any write,
  three retentions and seven reference-clock cases.
- [`checks/npu-startup.c`](../checks/npu-startup.c) runs it on two QEMU cores
  with real FreeRTOS tasks and the real counter, registry, ledger and register
  lock. The success mode reaches service activation; the readback mode shows a
  wrong readback retaining the register lock and every token.
- `checks/npu-application-clock-reference.py` drives the linked image's
  sequencer through its register pass for six reference-clock cases.
- `checks/npu-stop-native.py` exercises the linked allowlist adapter's stop
  writes, PLL rules and PCK600 refusals.
