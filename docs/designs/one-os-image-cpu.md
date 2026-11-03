# Design: CPU clock grants, A76 placement and performance modes at run time

Status: **proposed**, for the design panel (simplicity, hardware performance, correctness) before any code. This is series 2 of [one-os-image.md](one-os-image.md). It changes how the DCDC3 and DCDC5 rail writes and the CPU, DSU PLL writes are granted, so it is a voltage and power change under the review gate in `AGENTS.md`.

Readers are assumed to know [cpu-clock.md](../cpu-clock.md), [cpu-a76-dvfs.md](cpu-a76-dvfs.md), [performance-modes.md](performance-modes.md) and the foundation of one-os-image.md (the granted set `os_session.granted`, `os_granted()`, the refusal table in `apps/os/grants.c`).

## What the build options do today

| Option | Effect |
| --- | --- |
| `A733_A76_ADMITTED` | `apps/os/cpu.c` `os_cpu_admit` (read-only: CPU-B PLL locked and decodable, else the thermal start refuses); PLL page mappings in `thermal.c`; placement constants `A733_SMP_APPLICATION_CORE` 6 and `_WORKER_CORE` 7 (2 and 3 without); `application.c` places `cluster == SS_CLUSTER_A76` on core 6. Without it a manifest that asks for the A76 cluster is placed on core 2 without a message. It changes nothing in the port, scheduler, GIC or MMU, and cores 6 and 7 are released by the boot loop in every image. |
| `SUNSTONE_A733_OS_CPU_CLOCK` | The 96-byte grant record in the session; the owner state machine `os_cpu_clock_step` in the thermal task, which requests each granted cluster's maximum once and then lets the driver follow the thermal level; USB0 operation 7; `ss_clock_ready`, `ss_activity_*`, `ss_clock_info`. This is the only code that writes DCDC3, DCDC5 and the CPU, DSU PLLs. |
| `SUNSTONE_A733_OS_PERF`, `_PERF_BOOT` | `perf.c`, `a733_perf_mode.c`, the NPU and GPU hooks, USB0 operation 8, speed-bin admission. `OS_PERF_BOOT_MODE` (1 high, 2 low) also sets the GPU start rate and the NPU boot profile. |

## What protects the writes today

1. The compile flag: an image without `CPU_CLOCK` has no code that writes.
2. The grant record, checked at EL3 admission by `a733_cpu_clock_grant_load`: magic, version, size, nonzero owner; for a grant that names clusters, the board session's epoch and generation, 1 to 64 changes, each admitted cluster's maximum equal to a row of the driver's table, zero for the others, and a **nonzero** approval hash. A record with no clusters must be all zero.
3. The driver: a transition needs a grant bit and an unused change; every write goes through a clock lease and a rail lease; the thermal policy lowers clusters at THROTTLE.
4. The host preparation (`tools/board_images.py`): a grant whose maxima are not the reviewed points (CPU-L 1794 MHz, CPU-B 2002 MHz) is refused unless the preparer passes `--approved-by coordinator`.

**The approval hash proves nothing today.** `cpu-clock.md` and `os-image-history.md` say the hash identifies a record that binds board, image, session and step. I found no code that builds such a record or checks the hash: the image tests only that it is nonzero, and the session builders copy a template's bytes. The tool that the older design promises (`apps/os/prepare.py` writing the approval and B1 records) does not exist, and the grep for the symbols finds only documents. So the gate for the rail writes is layers 1, 3 and 4, and layer 2 is a format check. In one image, layer 1 disappears, so the gate has to carry more weight.

## Decision

### The manifest declares the need

A new block in `struct ss_manifest` (manifest version 2, 552 to 568 bytes; the generator, the host decoder, the link check and `checks/sunstone-app-abi.py` change with it):

| Field | Meaning |
| --- | --- |
| `clock_clusters` | Bits LITTLE=1 and BIG=2: the clusters whose clock the application asks to change. Zero: none. |
| `clock_max_mhz[2]` | The highest point asked for each declared cluster, a row of the driver's table; zero for an undeclared cluster. |
| `perf` | 0 none; 1 the performance-mode service with the high boot mode; 2 with the low boot mode. Needs both clusters declared. |

`cluster` (`a55` or `a76`, existing) stays the placement request.

The manifest is the application's request. It grants nothing: it is the upper bound of what a session may grant, and it makes the application's need visible to the review of the manifest.

### The session carries the grant and its approval

The grant record's layout does not change (96 bytes, extent type 4). Its `clusters`, `max_mhz` and `changes` must satisfy: `clusters ⊆ manifest.clock_clusters`, `max_mhz[d] ≤ manifest.clock_max_mhz[d]`. The grant therefore can lower what the application asks for, never raise it. A manifest that declares clusters while the session carries no record gets the read-only behaviour below, and `ss_clock_ready()` returns 1 at once, as an image without a grant does today.

**The approval hash gets a defined meaning.** Version 2 of the record: `approval` is the SHA-256 of

    "A733-CPU-GRANT-2" || image build ID (32) || manifest SHA-256 (32) || owner (8) || clusters (4) || max_mhz[2] (8) || changes (4)

all little-endian. EL3 admission recomputes it and compares. It is not a proof that a person approved the change (nothing inside the image can prove that). It binds the grant to this image build, this application manifest and these limits, so a grant prepared for another image, another application or other limits is refused, and the old per-variant binding (a CPU-clock image had its own build ID) survives into the one image. The human approval stays where it is: the board queue's `--approved-by coordinator` and the reviewed-points rule of `tools/board_images.py`, which records the approver in the run's evidence record. Version 1 records are refused; sessions prepared before this change must be prepared again. I judge that cheaper than carrying a second record format. The panel is asked whether it agrees.

Preparation writes the hash in one place, `tools/session.py` (the session builder that landed in bringup), and `tools/board_images.py` stops copying a template's approval bytes.

### The board description

A table in the board port (`platform/a733/board_caps.c`, a constant): the CPU clusters the board has with their core masks (A55 0x3f, A76 0xc0 for the A733), the DVFS speed-bin source (the SID word at 0x0300624c) and the cluster rails (DCDC5 for CPU-L and DSU, DCDC3 for CPU-B). The QEMU board port lists no A76 cores, which turns a host check of the refusal into a boot check. No run-time probing: the table is data of the port.

### Admission (EL3, `os_session_valid`)

Added to the existing grant check, in this order:

1. Version 2, magic, size, owner, epoch and generation as now.
2. The approval hash recomputation above.
3. `clusters ⊆ manifest.clock_clusters ⊆ board clusters`, and `max_mhz` within the manifest's maxima.
4. If `manifest.perf`: the grant names both clusters (today's rule that the performance service switches both, or none), and the SID speed bin has an entry in the performance table (today's check at the first switch, moved up so that a refusal happens before any write).
5. If `manifest.cluster == a76`: the board lists A76 cores. The CPU-B precondition (PLL locked at the boot value) is read-only and stays in `os_cpu_admit`, which now runs only for an A76 manifest and refuses the application's start instead of placing it on core 2.

A refusal names its reason in the existing decode sub-term space (`A733_DECODE_SERVICE`, plus new sub-terms for the manifest bound and the placement).

### Run time (EL1)

`os_cpu_clock_step` begins with `os_granted(OS_GRANT_CPU_CLOCK)` and returns 0 without a register access, **not even the read-only inspection**: an ungranted session reads nothing of the CPU clock tree. A record with `clusters == 0` (granted, nothing admitted) keeps today's inspection pass, which is read-only. `os_cpu_admit` runs only for an A76 manifest. The perf service is created only when `manifest.perf` and the grant name it; USB0 operations 7 and 8 answer "not provided" otherwise (today they are absent from the image; their op numbers are not reused). The GPU and NPU performance hooks are already guarded by `config.performance_begin` pointers in the GPU service and by `perf` state in `npu.c`; the series replaces `#ifdef SUNSTONE_A733_OS_PERF` there with the pointer being null, so a disabled service costs the null test it already pays.

The boot mode is read from `manifest.perf` where `OS_PERF_BOOT_MODE` is compiled in: `os_gpu_configuration` (the generated table), `npu.c` and `tools/sunstone-manifest.py` read the field instead.

### What stays the same

The driver, the lease and rail coordination, the thermal policy, the order of requests, the 2 us mux wait, the readbacks, the fault records. The series moves where the decision is made, not how a transition is performed.

## Measurable budgets

| Budget | Before | Limit after |
| --- | --- | --- |
| Register accesses of an ungranted session, CPU clock tree, rails and PMIC | not testable (code absent) | zero, by a trap in the host model (below) |
| Cost of an ungranted thermal step | no code | one load of `granted` |
| Thermal step with a grant | measured by `checks/os-thermal-cadence.py` | unchanged within the check's tolerance; the check runs before and after |
| Instructions of the GPU job path with performance modes off | `#ifdef` removed code | the null test already in `pvr_gpu_service.c`; the series counts the instructions of the submit and completion paths with `llvm-objdump` before and after and records both |
| Image size | `all` 867,100 bytes of text + rodata | within +2 KiB for the manifest block, the hash and the board table |
| Admission time | one SHA-256 over the session | plus one SHA-256 over 120 bytes |

An unexplained regression in any row blocks the series.

## Checks

- `checks/os-cpu-clock-owner.c` and `checks/cpu-clock-model.c` (existing models): add the manifest bound and the hash.
- New cases in `checks/os-session-grants.py`: a table of grants against manifests (equal, lower, higher, undeclared cluster, perf without both clusters, version 1, a hash for another image, another manifest, other limits); each refusal must come with its sub-term.
- Ungranted trap: the real `os_cpu_clock_step` against an ungranted session with the clock, rail and PMIC providers replaced by functions that stop the program; a mutation that removes the guard must fail it (as case 32 of `os-npu-service.py` does for the NPU).
- A76 placement: a manifest with `cluster = a76` against a board table without A76 is refused at admission; today's silent placement on core 2 is a failing case.
- `checks/perf-mode.py` against the manifest-selected boot mode, and `checks/os-cpu-clock-native.py`, run on the one image.
- Host tools: `tools/session.py` writes the hash; `tools/board_images.py` rejects a grant whose hash does not match; both are exercised by `checks/os-session-host.py`.

## Effects outside the OS

- The three variants `cpu-clock`, `perf` and `perf-npu-low` leave `VARIANTS`; the options `A733_A76_ADMITTED`, `SUNSTONE_A733_OS_CPU_CLOCK`, `_PERF` and `_PERF_BOOT` leave the CMake; the `ret-npu-load`, `ret-gpu` and `ret-latency` recipes drop three flags each.
- `include/a733_smp_placement.h` loses its `#ifdef`: the application core comes from the manifest's cluster (the generator already writes it into the GPU route and `os_gpu_configuration`), and the `_Static_assert`s in `apps/os/start.c` become the admission check.
- Sessions prepared for earlier images do not admit (version 2 record, manifest version 2). The runner's queued entries that carry a grant must be prepared again.
- The queue-mode NPU rule (the A76 cluster is refused because the ring's route is core 1) is an admission row of the refusal table (NPU queue with an A76 manifest), unchanged in effect.

## Questions for the panel

1. **Simplicity:** is the manifest block needed, or is the grant alone the declaration? Without the block, the application's need is not in the manifest the reviewer reads, and the perf mode has no home.
2. **Hardware performance:** the ungranted thermal step does one load and no register access; a granted session does what it does today. Is there a path where the one image costs a granted session more than the CPU-clock build does? I found none.
3. **Correctness:** the approval hash binds the grant to the image, manifest and limits and does not prove a person's approval. Is that the right meaning, given that EL3 cannot see the person? If the panel wants an approver identity in the record, the record gets a field and the queue's approver is written into it.
4. **Correctness:** version 1 records are refused outright. A transition period would keep the format check alive in one more place.
