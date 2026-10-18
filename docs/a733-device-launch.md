# Launch the larger device-memory image with an admitted reservation

Status: implemented host and SRAM launcher; focused host and instruction checks
pass. Physical launch is blocked by missing DRAM, entry and independent
recovery qualification. This interface is for the privileged board coordinator.

The launcher starts `sunstone-a733-smp-cached-devices` at `0x41000000` with a
version-2, 224-byte boot contract. The linked image reserves 768 MiB of write-back
memory at `0x48000000..0x78000000` and 16 MiB of non-cacheable memory at
`0x78000000..0x79000000`. These are half-open physical intervals. Both pools are
NOLOAD reservations: the uploader sends no bytes to them and does not clear them.
The image owns initialization of each allocation before a device can read it.

The host accepts only the exact linked profile and its file-backed build ID.
Pool sections must be writable, non-executable NOBITS sections with exact bounds.
A load segment may cover both adjacent pools, but must contain no file bytes.
Other segments remain inside `0x41000000..0x42200000`; executable bytes remain
below `0x41200000`, and file bytes cannot overlap the result or contract area.

## Bind the image and current memory ownership before execution

The image manifest must contain these fields alongside its source and build
input identities:

```json
{
  "target": "sunstone-a733-smp-cached-devices",
  "contract_version": 2,
  "execution_profile": "bounded-device-diagnostic-v2",
  "image_kind": "runtime",
  "build_id": "32-byte linked image identity as hexadecimal",
  "final_elf": "SHA-256 of the actual ELF"
}
```

`tools/check-a733-device-launch.py prepare` takes `--image`, `--image-manifest`,
`--output` and `--reservation`. Preparation compiles the separate SRAM launcher,
extracts only file-backed segments and generates a fresh epoch, generation and
boot lease. The reservation argument identifies the intended exclusive memory
reservation. Preparation does not qualify that reservation or access the board.

The six final contract words contain layout ID 1, reservation identity, write-back
start/end and non-cacheable start/end. The request repeats the layout and
reservation identity. The native launcher checks both records and the image
build ID before changing watchdog or reset registers. It publishes contract magic
only after arming and reading back the watchdog. Compact version-1 requests and
contracts are refused; their existing tools and sealed observer remain separate.

`launch` requires `--bundle`, `--fel`, `--qualification`, `--input-evidence`,
`--memory-admission`, `--database`, `--claim`, `--owner`, `--evidence` and
`--confirm-bridge-stable`. The existing board lock and claimed queue entry remain
mandatory. The queue review must bind the exact prepared bundle, ELF and launcher
and these prerequisite records:

- `independent-watchdog-rom-return`: the existing qualified recovery profile.
- `stable-protected-fel`: the current protected ROM-instance record.
- `device-memory-session-v2`: physical memory and entry admission for this launch.
- `device-launch-software-v2`: exact host, base helper, image, launcher and focused
  software evidence identities.

The memory record uses schema `a733-device-memory-session-v2`. Its `binding`
contains target SoC `00001903`, USB port, resolved sysfs path and current instance;
image SHA-256 and build ID; epoch, generation and boot lease; layout ID,
reservation identity and the three exact image/WB/NC intervals. Both
`physical_qualification` and `ownership_retained` must be true.

Each entry in `conditions` references a separately hashed record with schema
`a733-device-memory-condition-v2`. It must repeat the same binding, name its
condition, assert `physical_access` and `established`, and reference nonempty
physical observations. Required conditions are:

- `dram-initialized`: the complete admitted intervals are initialized RAM.
- `dual-pas-dram`: Secure and Nonsecure accesses reach the intended DRAM bytes.
- `dirty-alias-exclusion`: conflicting mappings and dirty cache aliases are resolved.
- `exclusive-cpus-memory-dma`: the coordinator owns the participating CPUs and memory.
- `remote-writers-excluded`: firmware and device writers cannot modify the reservation.
- `source-backed-cpu-dsu-transition`: the current CPU/cluster entry requirements hold.
- `physical-counter-continuity`: the admitted counter survives the launch transition.

An artifact reference contains an absolute `path` and its `sha256`. The host
checks every referenced file and the coordinator's reviewed prerequisite chain.
Hashes establish artifact identity; the physical observations and their review
establish qualification. The tool creates no qualifying records. Missing,
modeled, stale or mismatched evidence prevents upload. It revalidates the reviewed
admission immediately before execution and archives the referenced observations.
A nonzero reservation identity alone grants no memory authority.

## Preserve the bounded diagnostic lifetime

The SRAM launcher retains the four-second watchdog interval, one-second startup
bound, two-second diagnostic deadline and one-millisecond arm/transition bound.
It does not feed or disarm the watchdog after its initial arm sequence. A larger
pool does not extend those deadlines. Persistent operation requires a separate
reviewed supervisor grant and image lifetime profile; this tool supplies neither.
An unsafe architectural entry or a failure after reset-register mutation but
before watchdog arming can leave the CPU parked without an active watchdog.
Independent recovery prerequisites and retained coordinator ownership still apply.

Uploads use sealed byte sources and exact readback before one execution request.
A transport failure prevents further requests. Uncertain execution retains the
board lock and queue ownership for coordinator recovery. A valid refusal from
the original ROM instance permits the existing bounded SRAM restoration path.
A fresh ROM response establishes recovery observation only. This image has no
live result channel in the launcher, so scheduler and device results remain
unobserved; post-reset DRAM is never treated as execution evidence.

## Check only the changed launch paths

`checks/a733-device-launch.py` accepts the actual runtime ELF and manifest with
`--image`, `--image-manifest` and `--evidence`. It prepares the bundle once, checks
new range and evidence refusals, and composes the actual transfer helpers against
a finite model endpoint. The ARM instruction model checks version-2 publication
and each new pre-mutation refusal. The original reset failure matrix and scheduler
workloads are not replayed.

Physical condition fixtures exist only in test memory and are never emitted as
qualification records. Saved results identify the actual source, ELF, launcher
and modeled execution. Software checks cannot qualify DRAM, interconnect access,
watchdog recovery, coherence or a connected device.

The retained software evidence is
`~/.cache/sunstoneos/evidence/device-launch-20261017/batch1/results.json`.
It uses the actual production ELF from the device-capacity build, records twelve
launcher instruction cases, and confirms that transfers never reach either pool.
