# Measurements: board-test bundle preparation

These measurements check
[tools/prepare-board-bundles.py](../../tools/prepare-board-bundles.py) against
the budgets in its [design](board-bundles.md). All values come from this host
on 2026-10-22. The host check
[checks/board-bundles.py](../../checks/board-bundles.py) produced the values
after the change. No board was contacted. The tool is host Python and changes
no native source, so no firmware image was built at the base commit or after
the change.

## Method

- **Baseline.** Before the tool existed, a shell ran
  `bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /sys --tmpfs /tmp
  --tmpfs /run --bind <dir> <dir> --unshare-all --die-with-parent --clearenv`
  ten times with `/bin/sh -c :` and listed the same views inside and outside
  the sandbox. The host view is what a preparation module sees when it runs
  without the sandbox.
- **After.** The host check builds AArch64 ELF fixtures with `clang-21` and
  `ld.lld`, commits fixture preparation modules, two fixture runners and a
  copy of the supervised transport to a new Git repository, and runs the tool
  once on 24 fixture rows. A fixture module
  lists `/dev`, `/sys`, `/tmp`, `/run`, the network interfaces and the
  environment from its own process and from a child process, and tries to
  write outside the run directory. The check reads each row's `run.json`
  metrics, stat modes, snapshot manifest and submission. It times the tool's
  own sandbox prefix running `/bin/true` fifteen times.
- **Negative control.** With `--tmpfs /tmp` removed from the sandbox prefix,
  the check fails on the fixture's `/tmp` listing.

## Results

| Metric | Baseline | Budget | After |
| --- | --- | --- | --- |
| Entries visible to preparation and its children | 265 in `/dev`, 11 in `/sys`, 16,918 in `/tmp`, 69 in `/run`; 98 environment variables; host network | 14 `/dev` entries; empty `/sys`, `/tmp` and `/run`; 4 environment variables | 14 `/dev` entries; empty `/sys`, `/tmp` and `/run`; `PATH`, `HOME`, `LANG` and `PYTHONDONTWRITEBYTECODE`; loopback interface only; writes outside the run directory fail |
| Sandbox start time | Median 4 ms, range 4 to 6 ms, `bwrap` running `sh` | 100 ms | Median 2.9 ms, maximum 3.5 ms, `bwrap` running `/bin/true` |
| Bytes read from `bundle/` outside `prepare()` | Not applicable | The bundle size, one pass | Equal to the bundle size for each prepared row: 4,171,290 bytes for the nested fixture of five files |
| Writable entries in `bundle/` during the artifact pass | Not applicable | Zero | Zero |
| Copies of each bundle file | One | One | One; each snapshot holds the row's sources and `artifacts.json` only |
| Submissions refused by `submission()` | Not applicable | Zero | Zero of three prepared rows |
| Monitor images accepted that should be refused | Not applicable | Zero | Zero of ten injected cases: no symbol table, no `monitor_entry`, each of the seven prefixes, and a memory image without `monitor_renewal_cap` |
| Queue timeout | Not applicable | Monitor 1,200 s; Terminal the attempt bound, at most 3,600 s | Monitor 1,200 s. Nested fixture 1,118 s, which is its 1,058 s attempt bound recomputed with the transport's `transfer_timeout()` plus 60 s. Without uploads, 970 s for the OS-image rows and 930 s for the arbiter row. An existing OS-image bundle of 42 files and 22.9 MB: 2,451 s |
| Rows refused for the wrong build, no named admission values, no `COMPLETION_COUNTS` or a bound over 3,600 s | Not applicable | All | All six injected cases: a Monitor and a Terminal bundle admitted under another schema, a Monitor and a Terminal row without admission values, a runner without `COMPLETION_COUNTS`, and a 101-file bundle whose bound is 4,061 s |
| Host check wall time | Not applicable | None | 0.79 s |

The fixture run also refused a failing `prepare()`, an uncommitted source and
a missing module on their own rows only, and stopped unknown and excluded
input rows before creating a run directory.

## Not measured

The remaining budgets need real bundles or the board: DDR tested, memory runs
per chain, time from entry to forced return, fixed cost per run,
re-enumerations per chain and chain wall time. The memory profile and the
capped renewal handler do not exist yet, so no memory bundle can be prepared.
