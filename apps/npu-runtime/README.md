# Runtime input for the selected NPU model

This retained, single-core application accepts new UINT8 inputs through USB0
while the selected compiled model remains fixed. It returns the complete copied
numerical output after matching DONE and REAP. A caller can submit another input
in the same session, then request healthy NPU CLOSE. USB and PMIC tasks stay
alive to report that terminal result; this profile does not return to ROM.

Configure `apps/npu-convolution-single-core` with the existing toolchain and
selected model artifacts, adding `-DA733_NPU_RUNTIME_INPUT=ON` and
`-DA733_NPU_INTERRUPT=ON`. Do not define `SUNSTONE_NPU_FINITE_RETURN`.
The manifest profile is `npu-runtime-single-core-supervised-v1`. Existing finite
and retained reference-comparison profiles keep their respective behavior.
The model catalogue fixes input shape, output extent and quantization. The
selected `conv12` consumes 432 bytes and produces 400 bytes; the two-stage
`subsample2` graph consumes 64 bytes and produces eight.
The built-in input/reference arrays remain build-time artifact checks; runtime
inference does not compare arbitrary output against that canned reference.

Use `apps/npu-convolution/prepare-supervised.py` to prepare the exact image and
reviewed session. The immutable runtime marker and explicit manifest workload
prevent treating the reference-tested observer as this producer. Launch/capture
uses `tools/check-a733-npu-supervised.py` with one or more `--input FILE` options.
Each file must have the selected input extent. The host accepts at most sixteen
jobs per capture and loads and validates all local data before launch.
Optional `--oracle FILE` arguments must match every requested output extent;
they affect host comparison only. Ordinary capture requires no oracle.
Fresh board-specific authority is mandatory for physical execution. Software
fixtures are explicitly marked nonphysical and cannot supply that authority.

## One mailbox preserves input and result ownership

USB owns request assembly and response transmission. The original privileged
NPU client alone calls the existing convolution API. SUBMIT copies bounded
fragments into private CPU storage, checks identity and time, hashes the copied
input, then release-publishes one queued request. The client acquires it and
uses unchanged convolution input copying, command publication, matched completion,
output copying and REAP. Neither host pointers nor host command streams reach
the engine. There is no model upload, shape change or concurrent request queue.

The client release-publishes copied output only after REAP. A pending READ
callback cannot free its request. Only full TX completion of an explicit matching
DONE READ permits mailbox reuse. The response bytes remain immutable while
queued. Timeout, malformed framing or disconnect permanently fails the lifetime;
a late client publication cannot replace failure with DONE. A revoked response
is replaced if entirely unsent, or truncated if partly queued. Already queued
bytes cannot be recalled. The host requires the later healthy CLOSE result
before declaring the whole capture successful, and preserves incomplete fragments.

CLOSE requires the previous output response to have completed transmission.
The client sends service CLOSE, requires the existing interrupt-stop receipt,
and waits for the original bootstrap's local gate/reset shutdown before publishing
CLOSED. The service, allocations and shared leases retain session ownership.
The IRQ counter must equal one bootstrap completion plus successful jobs, with
wrap refused. This reuses the existing interrupt route, dispatcher, binding and
service; USB remains polled. No monitor, GIC or return code changes are required.

## The request and copied result have explicit wire identities

USB uses VID/PID `1f3a:efe8`, device revision `0104`, product
`sunstoneos NPU runtime input`, serial `sunstoneos-a733-npu-runtime`, OUT endpoint
2 and IN endpoint `81`. Each request starts with 64 little-endian bytes:

| Offset | Type | Meaning |
| --- | --- | --- |
| 0 | u32 | Existing observer magic `0x53534f53` |
| 4 | u32 | Strictly increasing envelope sequence, starting at one |
| 8 | u32 | SUBMIT `0x34`, READ `0x35`, CLOSE `0x36` |
| 12 | u32 | Fixed envelope payload length 48 |
| 16, 24 | u64 | Admitted epoch and generation |
| 32 | u64 | Nonzero request identity |
| 40 | u64 | Timeout in microseconds; zero for READ |
| 48 | u32 | Selected input extent for SUBMIT, zero otherwise |
| 52, 56 | u32, u64 | Zero flags and reserved word |

SUBMIT has exactly the declared input bytes after the header. Its timeout and
CLOSE's timeout must be at most five seconds; their actual deadline is capped
by the twenty-second session. READ names the outstanding request. New SUBMIT
and CLOSE identities must increase. Partial header assembly remains bounded by
the session, while payload assembly and unread results share the request deadline.

Every response is a 16-byte observer envelope with command `0x80000034` and
1,024-byte payload. The payload starts with magic `0x415337334e505232`, version
one and its explicit size. It binds state, epoch, generation, lease, image,
request identity, deadline and selected-model digest. DONE adds start/end times,
job identity, successful-job and IRQ counts, input/output digests and exact copied
output. CLOSED carries the shutdown result without numerical bytes. Pending and
failed responses carry no numerical result. A SHA-256 digest covers the first
992 payload bytes; it establishes byte identity, not authentication.

## Focused software evidence

Evidence is under `~/.cache/sunstoneos/evidence/npu-runtime-input-20261019/`.
One actual linked USB/mailbox/client sequence supplies two different inputs,
collects both outputs, reaps both jobs and completes healthy CLOSE. Service
replies, generated output, startup health, stop and scheduling remain modeled.
Injected output tests copying and lifetime; it does not establish inference.
Nine focused cases cover malformed/stale requests, incomplete upload expiry,
pending callback ownership, unsent/partial success revocation, retained collection
timeout and uncompleted local stop. A final isolated branch check covers capping
the request deadline to the remaining session. The unchanged paths reuse the
accepted preceding runtime result through the saved object/function comparison.

Production preparation, explicit runtime identity, ordinary output without an
oracle, fragmented durable host capture and partial transport evidence pass.
Astra review closed the staged-success and host validation/evidence findings.
Root reviewed the unchanged IRQ architecture composition against the final image.
Prior compiler, model, controller and route suites were not rerun. Physical
numerical inference, interrupt delivery, signaling and recovery remain unqualified.


The two-stage graph's selected native image and focused results are described in
[the graph support record](../../docs/npu-two-stage.md). It uses this same live
input, copied result and healthy CLOSE protocol, with one completion per complete
graph job. Its software checks do not establish physical inference or interrupts.
