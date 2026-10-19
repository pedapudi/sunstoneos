# One direct endpoint DMA COPY with retained native ownership

The separate `sunstone-a733-pcie-test` application implements selected BAR
allocation, control-register MMIO and one 4096-byte endpoint-DMA COPY through
the pinned `pci-epf-test` protocol. It reuses the existing COMB1, PCIe,
DCDC2/BLDO1, PCK, clock, pin, PMIC and register-mutex providers. FreeRTOS runs
on A733. Linux and U-Boot supply protocol and register references. No endpoint
is assumed connected; physical operation remains unqualified.

The configuration-only application retains its BME-clear and interrupt-masked
READY policy. The test client claims a READY port once, reestablishes function
zero and cold endpoint identity under the existing mutex, then prevents the
configuration-only APIs from advancing that port. One privileged task owns the
entire transaction. USB0 receives an immutable copied report.

## BAR and MSI programming remain bounded

The client examines all six BAR slots as 32-bit or paired 64-bit memory BARs.
It rejects inherited addresses, an upper-half selection, I/O/reserved types,
malformed sizing masks and allocations larger than 1 MiB. Only the admitted
control BAR is assigned. Outbound window zero remains CFG0 at `0x20000000`.
Window one maps `0x22000000..0x220fffff` as memory transactions, matching the
pinned DTS memory resource. The root memory window covers that same interval.
Device mappings, enablement and programmed addresses receive readback checks.
Only the endpoint protocol's MAGIC word is written during the MMIO check.

The native adapter checks the actual owner task, privilege and register-mutex
holder. Claimed-mode writes also pass the concrete client's address/value
policy, including registers admitted by the original configuration adapter.
Root and endpoint BME enablement are explicit transaction-mode operations.
PCI command/control accesses preserve adjacent status bits by using 16-bit
writes. The client requires disabled Relaxed Ordering, No Snoop and supported
IDO controls, then configures one MSI message with endpoint IRQ number one.
MSI-X and legacy interrupt delivery remain disabled.

The root-owned route uses SPI 153, absolute INTID 185, level high. Its ISR issues
a permanent mask after the first event and publishes metadata. The owner polls
that event, reads and acknowledges the exact controller MSI status under its
normal mutex, and checks the endpoint result. There is no successful rearm.
A GIC disable command does not prove propagation, endpoint shutdown or DMA drain.

## Completion preserves all memory and resource ownership

The existing uncached pool supplies seven private pages: three source pages,
three destination pages and one MSI-target page. COPY accesses only each
source/destination allocation's middle page. The surrounding pages are guards.
The owner initializes all bytes before device ownership. The native MMIO
adapter's barriers order those writes before enabling BME and submitting the
command. CPU and device addresses are checked identity mappings within the
image-owned pool; no runtime attribute transition is introduced.

The endpoint receives exactly one COPY command with `FLAG_USE_DMA=1`. Its
source and destination are disjoint. Successful publication requires one new
MSI event, exact COPY-success/IRQ-raised status, cleared command, healthy x1 Gen1
links and resources, independently expected source/data/guard bytes, and a
checked copy into private application backing. The source pattern depends on
the session generation. Comparing destination bytes only with mutable source
bytes would not establish integrity.

The 840-byte sealed session binds the original board/PCIe authority, selected
BAR, operation bound and endpoint-proof digest. The proof identifies actual
controller and DMA source revisions, selected TX/RX channels and configuration.
It must establish the pinned function's COPY-capable RX path, TX/RX MEMCPY
capabilities and nonprivate DMA mode. `dma_supported` alone is insufficient.

The pinned function clears COMMAND before executing work. Its COPY path removes
endpoint outbound mappings before publishing status and raising MSI. Physical
admission must therefore prove DMA completion before that removal, and data
visibility before status/MSI, for the actual endpoint implementation. Neither
COMMAND clear, a delay, MSI alone nor link-down establishes these conditions.
Those endpoint-specific proofs are unavailable without an identified endpoint.

Timeout, missing/duplicate MSI, bad status, damaged guards, link/resource failure
or uncertainty closes output admission. Every page, translation window, pin,
PCK, rail and clock lease remains owned until reviewed whole-board and endpoint
recovery. Successful copy also retains allocations permanently. The owner keeps
observing controller/link state; USB0 checks a bounded published observation
through transmission. Software deadlines cannot recover a stalled physical MMIO
access. Disabled inbound windows forward identity addresses and supply no
hostile-endpoint isolation.

## Build, host capture and evidence

Enable `SUNSTONE_BOARD_SUPERVISED=ON` and `SUNSTONE_A733_PCIE_TEST=ON` with the
existing pinned native dependencies. Build `sunstone-a733-pcie-test`.
`apps/pcie-test/prepare.py` reuses board admission, launcher construction and
frozen-input checks while requiring the additional endpoint proof.
`capture.py` uses the existing queue, target lock and retained-failure transport.
It validates and durably saves a 4224-byte report: 128 bytes of transaction
metadata followed by the copied 4096 bytes. The USB header binds the full session
and report hashes. Pending and failed responses carry no payload.

Software evidence is under
`~/.cache/sunstoneos/evidence/pcie-endpoint-20261019/`. The client sanitizer
cases execute actual shared providers and the new BAR/iATU/MSI/copy path.
The linked instruction cases execute actual native value/mutex checks,
application copy/publication and fragmented USB serialization. Host cases check
specific endpoint-proof refusals, copied-data integrity, durable capture,
transport failure and a nondeployable frozen launcher bundle. Controller
responses, DMA, MSI delivery, initial manager metadata, translations, scheduling
and transport are modeled where stated by each fixture. Earlier PHY/power
results remain authoritative within their unchanged scopes.

The separate architecture review covers linked route/dispatch behavior and
actual generated boot ordering. It does not establish CPU startup, scheduling,
cache visibility, physical PCIe operation or recovery. The candidate ELF is
`ac5c80a2dda437f31fadc325460ef8259c117aa43aefd8a1926b51232f86c2f4`.

`apps/pcie-test/source.json` pins the cached Linux endpoint/host protocol and
U-Boot BAR references. The A733 manual section 18.3.5.2 describes identity
forwarding when inbound translation is disabled. The pinned DTS at kernel
revision `2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f`, lines 5046–5071, supplies
the selected memory resource and MSI trigger/number. The kernel root-port MSI
handler clears controller status before dispatch.

This bounded client leaves bridge enumeration, general BAR/resource allocation,
other endpoint drivers, root embedded DMA, INTx/MSI-X, Gen2/Gen3, power transitions
and shutdown outstanding. Preserve both microSD cards. No hardware launch is
admitted while board recovery and the actual endpoint prerequisites are absent.
