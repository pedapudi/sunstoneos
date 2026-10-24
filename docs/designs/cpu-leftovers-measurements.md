# Measurements: one table publication fewer and a host reader for the fault report

Status: **measured in host models**; not run on the board. These figures
compare the budgets in [cpu-leftovers.md](cpu-leftovers.md) with two builds.
No figure here describes physical cache or coherence behaviour.

## Conditions

- **Base.** Commit `8e40a70`, the integration commit that this change starts
  from, exported with `git archive` into its own directory.
- **Head.** The commit that adds this document, with the edits outside its
  owned files applied: the measurement in `checks/cpu-table-walks.py`, the
  three capacity fixtures, the launcher flag and its check, the comment in
  `platform/a733/cpu_coherence.S` and the two component documents. Those
  edits are delivered as a separate patch for the integrating agent.
- **Images.** Every central target that links
  `platform/a733-smp-cached/continue.c`: the cached diagnostic, observer and
  supervised eight-core images, and the board, camera, GPU render, HDMI,
  mimus native, cached-devices and storage-maintenance images. The
  supervised image is the only one that links
  `platform/a733-smp-supervised/publication.S`. Also the integrated OS image
  in its resident and supervised lifetimes, and the QEMU kernel check and
  eight-core soak images. All were built with Clang 21.1.8 and `ld.lld`.
- **Tools.** `checks/cpu-table-walks.py` from the head tree, run with
  `--baseline` on base images. It runs `a733_cached_prepare()` from the
  linked image in Unicorn with its callees stubbed and records the range of
  each `a733_cpu_coherence_publish()` call. `checks/device-memory-capacity.py`
  ran from each tree on that tree's fixtures; QEMU 10.2.1 executed them.

## Budget metrics

| Metric | Budget | Base | Head | Within budget |
| --- | --- | --- | --- | --- |
| Table-section `DC CVAC` in `a733_cached_prepare()`, diagnostic image | 0 lines; no call names `__tables_start` | 321 lines, 3 publish calls | 0 lines, 2 publish calls | yes |
| The same, supervised image | 0 lines | 449 lines, 3 publish calls | 0 lines, 2 publish calls | yes |
| Table-line maintenance per boot, diagnostic image | 1,345 | 1,666 | 1,345 | yes |
| Table-line maintenance per boot, supervised image | 1,729 | 2,178 | 1,729 | yes |
| Lines that `a733_supervised_ns_publish()` maintains | 42,172 | 42,172 | 42,172 | yes |
| EL1 table `DC CIVAC` per core before `SCTLR_EL1.M` | 128 | 128 | 128 | yes |
| Pool translations; EL1 map | 392 `AT`; golden map | 392 `AT`; golden map passes | 392 `AT`; golden map passes | yes |
| Operation 5 response on the device | 1,160 bytes in 1 tick | 1 tick, 3 packets | 1 tick, 3 packets | yes |
| Host transfers per report | 1 request, at most 3 reads at 512-byte fragments, 0 retries | none | 1 request, 3 reads, 0 retries | yes |
| Copies per report byte on the host | one | none | one | yes |

Notes on the rows:

- The per-boot totals add the EL3 flush of the table section, the removed
  publication, and the 128-line EL1 flush on each of eight cores. The
  supervised image adds the 128-line `DC CVAC` of its two temporary EL3
  pages and the 128-line Non-secure `DC IVAC` of the EL1 pages in
  `publication.S`. Base: 321 + 321 + 8 × 128 = 1,666 and
  449 + 449 + 128 + 128 + 8 × 128 = 2,178. Head: 321 + 8 × 128 = 1,345 and
  449 + 128 + 128 + 8 × 128 = 1,729. The EL3 flush counts (321 and 449) and
  the EL1 flush count (128) are the same in both builds.
- The two remaining publish calls in every image are code at `0x41000000`
  (2 MiB) and initial RAM from `0x41200000` to `__bss_end`. A disassembly
  scan of `a733_cached_prepare()` finds three publish calls and one address
  load of the table page in each of the eleven base images, and two calls
  and no table-page load in each head image.
- `checks/cpu-table-walks.py` cannot model the camera, GPU render, mimus
  native and cached-devices images: two of them repeat a structure name with
  different layouts across compilation units, and two use the extended
  device-memory profile that its preparation model does not cover. The
  disassembly scan above covers those four images.
- The operation 5 row comes from `checks/usb0-service-burst.py` on the head
  tree. `apps/mimus-board/usb0.c` is unchanged, so the device side of the
  exchange is unchanged.
- The host rows come from `checks/fault-report.py`. With 512-byte fragments
  `fetch()` reads 512, 512 and 136 bytes in three reads, requesting 1,536,
  1,024 and 512 bytes. With 29-byte fragments it needs 40 reads and with
  1-byte fragments 1,160; the read count follows the device's fragment size.
  The fragments are appended to one `bytearray`, and `retain()` and the
  caller receive the same read-only view of it.

## Other costs

| Metric | Base | Head | Cause |
| --- | --- | --- | --- |
| `a733_cached_prepare()` size, eight-core images | 296 B (324 B supervised) | 260 B (288 B supervised) | the third publish call and its arguments, 9 instructions |
| `.text`, every rebuilt image | — | unchanged | the 36 bytes fall inside existing alignment padding |
| Estimated boot time saved, 70 cycles per operation at 494 MHz | — | about 0.05 ms (diagnostic), 0.06 ms (supervised) | 321 and 449 fewer `DC CVAC` |
| Builder instructions in the capacity fixtures, from QEMU traces | 20,785; 15,668; 987 | 20,795; 15,663; 984 | fixture layout, see below |

The capacity fixtures no longer publish the table section, so their code
and data move. The builder functions (`a733_cached_memory_prepare()`,
`a733_cpu_coherence_tables()`, `a733_cpu_coherence_flush()` and
`a733_memory_local_prepare()`) disassemble to the same instructions in both
builds apart from addresses. `checks/device-memory-capacity.py` derives
instruction counts from QEMU execution traces, and three of them moved by −5
to +10 instructions. They are the builder with the decode check passing, the
builder of the read-only-view fixture with the check failing, and the local
pool preparation of the uncached fixture. The `AT`, `DC`, `TLBI`, `DSB`,
`DMB` and `ISB` counts in every traced case are unchanged, as are the exact
PMU count of the publication run (22,541 instructions), the 392 translations,
the EL1 map size and all 13 native cases.

## Checks run on the head

| Check | Result |
| --- | --- |
| `checks/cpu-table-walks.py` on the diagnostic image | pass, 8 cases, including the new publication case |
| `checks/a733-cpu-coherence.py` | pass, 41 native and reference cases |
| `checks/a733-supervised-publication.py`, default and `--all-core-transition-only` | pass; all eight modeled cores reach cached EL1 |
| `checks/a733-smp-cached.py` on the diagnostic image | pass; eight-core scheduler and missing-secondary refusal |
| `checks/device-memory-capacity.py` | pass, 13 native cases |
| `checks/cached-device-reservations.py` | pass, eight cores |
| `checks/fault-report.py` | pass, 15 cases |
| `checks/os-image-launch.py` on the supervised OS image | pass, 21 cases, four of them for `--fault-report` |
| `checks/usb0-service-burst.py` | pass |
| `tools/check-kernel.py` | pass |
| `tools/check-smp.py --case soak8` | pass, one trial |

`checks/fault-report.py` was also run against altered copies of
`tools/fault-report.py`: without the size limit, the empty-fragment test,
the read rounding, any one of the identity checks, the final deadline test,
or the single buffer. Each altered copy failed the check.
