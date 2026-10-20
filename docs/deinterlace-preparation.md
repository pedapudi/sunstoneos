# Native DI301 preparation and capture

The host path prepares one fixed 640 × 480 NV12 frame for the A733 DI301
engine under FreeRTOS. NV12 stores the full-resolution Y plane followed by
interleaved U/V samples at half vertical resolution. Both plane strides are
640 bytes; the complete input and copied output each contain 460800 bytes.
The combined frame interleaves top and bottom field lines in each plane.
Offline WEAVE preserves those lines in the output.

`apps/deinterlace/producer.py` seals the input digest and four typed authority
records into an 888-byte session. The required configuration is top field first,
NV12, WEAVE, the inherited 300 MHz DI module clock, both IOMMU gate values
`0x10007`, and the shared DCDC2 rail at 800 mV. The reference digest stays zero.
The deterministic field pattern distinguishes the fields and plane rows:

```sh
python3 apps/deinterlace/producer.py --pattern-output /absolute/cache/input.nv12
```

`apps/deinterlace/prepare.py` admits an immutable native ELF and its tested
manifest. It checks the linked session geometry, protected memory ranges,
actual allocation rows and every IOMMU table descriptor. The two cached
allocations hold authenticated input staging and immutable copied output.
The uncached allocations hold the page table and guarded DMA input/output.
Only the source and destination spans are mapped; source pages are read-only.
The engine receives one authenticated upload with the admitted input digest.

Physical admission requires typed records bound to the image, target and
session. They cover prior DMA exclusion and draining, clock and power ownership,
private memory, device mappings, DMA visibility and reviewed whole-board power
removal. Decoded clock settings require established parent stability and ownership.
A model record cannot authorize physical execution. An offline fixture may replace
only the physical-authority check and must label all resulting artifacts as models.

`apps/deinterlace/observe.py` uses the existing supervised entry and queue policy.
The native lifetime is 20 seconds; host capture has 19 seconds. The USB identity
is product `sunstoneos native deinterlace`, serial `sunstoneos-a733-deinterlace`,
and device version `0x0100`. Entry validates the frozen bundle and matching
reviewed software evidence before using the shared supervisor.

`apps/deinterlace/capture.py` receives a 192-byte result header and, on success,
the complete copied NV12 frame. It checks the epoch, generation, input/session
hashes, fixed geometry, matched finish, cleared busy bit, DI master bitmap,
guards and copied-output hash. Reserved result bytes must be zero. Full-speed
and high-speed responses require complete short-packet or zero-length-packet
termination; truncated streams fail capture.

Capture durably saves the result and output, compares WEAVE output with the
sealed input, and journals acknowledgment intent before sending the 41-byte
acknowledgment. It requires a matching acknowledgment receipt before reporting
observed success. A transport or durability failure preserves an unobserved
result and the available journal. Output acknowledgment does not release memory,
clock, power or DMA ownership. Resources remain retained until the reviewed
whole-board power removal.

The focused checks are `checks/deinterlace-host-preparation.py` and
`checks/deinterlace-host.py`. Preparation consumes allocation and table bytes
from the actual native constructor fixture. Capture consumes an actual native
reply produced for the final prepared session. Mutated replies and transport
faults test refusal and acknowledgment ordering. These checks establish software
behavior; physical clock, DMA visibility, completion and recovery evidence
remain required before hardware qualification.
