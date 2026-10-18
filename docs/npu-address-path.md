# Address routing for the trusted first NPU convolution

The existing page-directory MMU is the intended external-memory confinement
method for the fixed trusted convolution. Static inspection of the pinned
revision-9.2 simulator connects descriptor, coefficient, input and output
accesses to its translated neural-engine client. The native directory value
excludes the simulator's flat-address mode. This is source/artifact review of
expected behavior; physical translation, permissions and inference remain unqualified.

The audience is the coordinator reviewing a supervised RAM-only NPU attempt.
Addresses below are ELF virtual instruction addresses in the inspected host
library, not board register addresses. Simulator code is a development reference
and does not run on the FreeRTOS target.

## The evidence binds the selected model and target decoder

The accepted frozen application selects the 12×12 convolution with four output
channels. Its model SHA-256 is
`d8de2e7a886741959d7806783e0a4bf96addc726f6c18804a420bbf4a250d3d9`.
Its descriptor occupies file bytes `1244..1436`; its coefficient section contains
384 bytes. The input contains 432 bytes and the copied output contains 400 bytes.
The existing immutable model catalogue, relocation policy and publication digests
bind the complete descriptor and coefficient bytes, including allocation padding.

The reference is the existing compiler image's `libEmulator.so`, SHA-256
`2d102725286968ffa4b011dfb7f92ca9f4434a71fd19920beaf4045923bbac6a`.
The [stride investigation](nbg-strides.md) already establishes selection of
`cmoNeuralNetV92` for the target identity. This inspection uses its actual
revision-9.2 overrides, rather than the older base-class implementations.
The compiler image and licensing are recorded in [npu-compiler.md](npu-compiler.md).
No new runtime dependency or model compiler was introduced.

## External neural accesses select the translated client

The simulator's client-name table at `0x5577e0` identifies client 1 as `fe`
(command fetch), client 16 as `nn` (neural engine) and client 17 as `tp`
(tensor processor). The dispatch table at `0x1da3c0` sends all three to
`cmoMMU::cmoClient::Translate` through `0x1a2193`. They do not select its
untranslated-client return at `0x1a21ae`.

The selected neural paths use client 16 at every external-memory call site:

| Selected path | External-memory call instructions |
| --- | --- |
| Descriptor fetch, `GetNnInst` / `ReadExtNNCommand` | `0x15846a`, `0x14f35b` |
| Coefficient fetch, `ReadNext64BytesKernelData` | `0x14beb1`, `0x14c283` |
| Input fill, `InputImageLocalReadBurst` | `0x154e08`, `0x154f2b` |
| Output flush, `FlushOutputBuffer` | `0x16c4c3`, `0x16c605`, `0x16c64f`, `0x16c6b9` |

The descriptor reader fetches 128 bytes initially and, for extended command
mode, another 128 bytes. This can read beyond the 192-byte descriptor, but both
fetches remain inside its dedicated 4 KiB allocation and deterministic padding.
The native controller also reserves its complete command-prefetch span.

Each read-burst call supplies zero for its final bypass argument. Its nonzero
first byte argument (`r9 = 1`) selects the ordinary per-word read path; it does
not disable MMU translation. The burst path reaches the `AQMEMORY::Read` virtual
slot at `0x18796c`. The relocation at `0x410288` identifies that slot as
`AQMEMORY::Read` at `0x1860c0`. That function invokes MMU translation at
`0x186122` for a nonzero client and a zero final bypass argument.

Output word calls supply client 16 and zero for their last argument. Output
bursts also supply zero for their final argument and eventually invoke the
word-write slot at `0x187ca4`. The relocation at `0x410290` identifies it as
`AQMEMORY::Write` at `0x186210`; it invokes MMU translation at `0x186262`.
The simulator-wide `StateParser::disable_mmu` option is a host configuration
escape, not a command-descriptor field. Its GOT relocation is at `0x4109d0`.
This reference option is distinct from the native driver's actual MMU registers.

## Native configuration excludes the observed flat-address route

The simulator decodes register `+0x388` at `0x1a152e`. Bit 0 enables MMU
translation and bit 5 selects page-directory mode. Native startup writes `0x21`
and checks the complete word before triggering and accepting completion.

The directory register `+0x3b4` is decoded at `0x1a159c` and `0x1a114a`.
Its low two bits select the addressing mode; the simulator's flat route requires
value 2 (`0x1a2162` and `0x1a1cdc`). The native builder forms the directory value
as `(physical_root >> 12) << 4 | 1`. The controller checks the complete value
before triggering and accepting completion, so the observed flat mode is refused.
Neither bootstrap nor the fixed convolution command template writes a different
MMU enable, directory or addressing mode during execution.

The pinned board driver's
[`vipdrv_hw_pdmode_setup_mmu`](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/npu/aw_nna_vip/vip2/vip_drv_hardware.c)
programs the same directory encoding and literal `0x21` control word. Its
[`mmu_set_page`](https://github.com/orangepi-xunlong/linux-orangepi/blob/2ac08e8c7cdc28abbdc5c9a9dd812f887ae9c79f/bsp/drivers/npu/aw_nna_vip/vip2/memory/vip_drv_mmu.c)
provides the native leaf permission/address encoding. Native publication checks
the entire private table image before cleaning it to the point of coherency.
Only the output allocation is writable through the ordinary external-memory map;
code, stacks, MMIO, recovery data and the table arena have no payload mappings.

The exception-enable bit is necessary to that containment argument. The simulator
has safe-address fallbacks for invalid directory/leaf entries at `0x1a1ecb`
and `0x1a2055`, and for denied writes at `0x1a20b3`. Clearing entry bit 1
can select those fallbacks without first stopping on an exception. Native absent
entries contain `2`; present read-only leaves end in `3` and writable leaves
in `7`. Thus each has bit 1 set. At `0x1a1e33`, `0x1a1fb8` and `0x1a208b`,
the simulator takes its exception path, raises event 30 and waits for fault
resolution before returning a fallback address. The native controller treats
that event as a retained terminal fault and supplies no resume or page repair.
This review does not claim that an arbitrary safe-address register value would
be harmless if exception behavior were disabled or a fault were resumed.

With the reviewed routing and effective MMU behavior, an unexpected coefficient
traversal or prefetch outside a readable mapping faults instead of accessing an
unrelated physical address. An output overrun outside its writable mapping also
faults. An overrun within its mapped 4 KiB output allocation remains owned by the
same session. Complete compressed-stream interpretation is therefore unnecessary
for confinement in this fixed mode. It remains relevant to numerical correctness,
capacity planning and future generalized model admission.

## Internal SRAM remains a separate owned route

The selected descriptor disables both tensor circular rings: words 18 and 20
have zero low 26 bits, and words 19 and 21 contain the disabled-end sentinel.
The descriptor's kernel cache is `0x18200..0x18c00` and its image cache is
`0x18000..0x18200`, both inside the 512 KiB internal SRAM. These cache addresses
are SRAM offsets, not ordinary mapped physical addresses. The kernel reader also
contains explicit internal-SRAM section calls alongside its translated external
reads. The fixed bootstrap supplies internal SRAM base `0x400000` and zero AXI
SRAM base/end, matching the pinned target's zero external-SRAM configuration.

The whole internal SRAM and its CPU alias must remain reserved, abandoned by
other users and powered for the session. SRAM ownership covers internal cache
accesses without treating their absent ordinary RAM leaves as protection.
This review does not establish SRAM routing or retention on the physical board.

## Launch admission can review this method without claiming qualification

The concrete confinement method is private page-directory translation for
external FE/NN accesses plus complete ownership of internal SRAM. The existing
trusted command stream selects no observed untranslated external route. This
supports a source/artifact-backed confinement review for the first trusted
workload; it does not require successful physical inference or hostile-client
isolation as prior evidence.

The coordinator can bind this review to the exact image/model, actual parent
allocation, verified table image, directory/control readback, SRAM abandonment
and retained clock/power/recovery session. The initial supervised attempt then
tests the expected hardware behavior. A software fixture or this report cannot
supply observed board state or assert already-qualified physical isolation.
The current admission flags and host parser are unchanged by this review; the
coordinator must explicitly determine whether the reviewed method satisfies
that session's confinement condition before preparing its immutable records.

Residual first-attempt risks are silicon behavior differing from the reference,
failed cache publication, a table walk or bus transaction stalling, an inherited
remote writer, and incorrect SRAM or power assumptions. Existing exact register
checks, exclusive ownership, event/error and full-idle checks, bounded software
polling and retained whole-board power-removal recovery address those risks
within their documented limits. A CPU/interconnect stall can defeat software
polling. Successful output alone will not qualify fault isolation.

Static inspection records and disassembly are preserved under
`~/.cache/sunstoneos/evidence/npu-address-path-20261018/`. `inspect.py` checks the
pinned hashes, client dispatch, selected descriptor/ring bytes and instruction
anchors, then records `inspection.json`. No simulator instruction, numerical
inference, device request, build or performance run was executed for this review.
