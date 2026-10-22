# Board-test bundles for the memory, GPU and NPU paths

Status: designed; not implemented. The bundles run only on the board, so
every path they carry stays implemented and host-modeled, with physical
behavior pending, until a scheduled session records the evidence named in
its row.

One host tool, `tools/prepare-board-bundles.py`, turns the build output of
each new memory, GPU and NPU path into a frozen bundle and a board-test
queue submission. The tool never contacts the board. The ranked table in
[docs/audit/board-test-plan.md](../audit/board-test-plan.md) lists the
bundles in order of their risk of losing the USB connection.

## Bundle table

A constant table in the tool holds one row per bundle:

- the path's existing preparation module (an `apps/*/prepare.py`) and the
  arguments its `prepare()` function takes;
- the recovery class (defined below);
- the queue prerequisites, expected result, timeout and stop condition;
- the item from the path's `docs/hardware.md` row that is still required,
  and the board evidence that would confirm it.

The plan's ranked table is generated from this table. `--write-plan`
rewrites the table between two marker comments in the plan, and the check
fails when the two differ. The plan's prose sections stay hand-written.

The recovery classes extend the three ways the plan already lists for the
board to return to the boot ROM's USB recovery mode:

- **ROM.** The boot ROM keeps the USB controller.
- **Monitor.** A RAM image runs under the EL3 monitor, whose secure timer
  forces a return. A bus stall is still unrecoverable.
- **Native USB.** The image takes the USB0 controller. Any failure needs a
  power cycle. Such a test ranks after rank 9, the native USB channel test.
- **Excluded.** The test needs a power cycle, an accessory, a voltage or CPU
  clock change, or an intentional hang. The plan lists it under the tests
  that wait for physical recovery, and the tool prepares no bundle for it.

The tool reads the image's symbol table and refuses a row whose declared
class is lower than the image's. An image that defines `dcd_int_handler`
contains the native USB device stack and is at least Native USB.

Rows appear as the owning packages land. Their proposed positions among the
plan's existing ranks:

| Position | Bundle | Class | Board evidence that confirms it |
| --- | --- | --- | --- |
| After rank 6, before rank 7 | Memory qualification image, if it is single-core and returns through the monitor | Monitor | Every tested DDR block reads back its pattern at the recorded CPU and DDR clocks; die and DDR temperatures stay below the 85 °C trip |
| After rank 9 | NPU service in the OS image | Native USB | One admitted model's output matches its host reference; the session closes and reopens without retention |
| Next | GPU service in the OS image | Native USB | Firmware boots, a compute job matches its reference and the service stops |
| Next | GPU and NPU jobs together under the memory and power arbiter | Native USB | Interleaved jobs match their references; the arbiter's grant record matches the host model |
| Next | Recognition dispatcher | Native USB | One recognition window runs through the dispatcher and matches the host result |
| Excluded | GPU and NPU hang reset, voltage and clock changes, display output, multicore memory qualification | Excluded | Waits for physical recovery or an attached display |

NPU ranks before GPU because GPU firmware writes memory on its own
([docs/remote-writers.md](../remote-writers.md)), so a GPU failure has more
causes to separate. A multicore memory qualification image ranks after rank
8.

## Data flow and ownership

1. **Inputs.** Each owning package builds its image under
   `~/.cache/sunstoneos/build/<package>/`. The tool builds nothing. It refuses
   a build directory that changes while it is read and a repository whose
   listed sources differ from `HEAD`.
2. **Preparation.** For each row, the tool creates a fresh directory under
   `~/.cache/sunstoneos/evidence/board-bundles-<date>/<bundle>/`; creation
   fails if the directory exists. It calls the path's `prepare()` into that
   directory, then the path's `validate_bundle()`. That function is the
   path's existing admission check: it refuses a bundle whose image, session,
   launcher, layout or supervision conditions differ from what the path
   accepts. The tool adds no admission rule of its own.
3. **Freezing.** The tool runs `tools/evidence-snapshot.py` once per bundle.
   The arguments are the bundle's files as artifacts and the path's source
   list as sources, the same list its supervised check already names. Each
   later snapshot passes `--reuse` with the earlier snapshots, so a file
   with an unchanged hash is hard-linked instead of copied.
4. **Submission.** The tool writes `submission.json` in the queue's
   documented schema. Each artifact's path is the snapshot copy, and its
   SHA-256 comes from the snapshot manifest, so the tool reads no file again
   to hash it. Sources carry the full 40-digit revision.
5. **Hand-off.** The coordinator submits, reviews, claims and runs each
   bundle with the existing queue and supervised runner. After the manifest
   is written, no process writes to the bundle directory.

## Interrupts and polling

The tool runs once and exits. It does not wait on the queue or the board,
and it polls nothing. On the board, every bundle uses its path's existing
supervised runner and the shared USB0 loop. That loop polls once per tick
because USB0 interrupt delivery is not qualified, a wait that section 1.3 of
[docs/performance-architecture.md](../performance-architecture.md) permits.
Where a path completes on a device interrupt, its row names that interrupt
in the expected result, so the session records whether it was delivered.

## Memory and caches

Bundles are small: the frozen eight-core launch bundle,
`a733-smp-launch-20261016/prepared-final`, is 548 KiB. Files are streamed in
1 MiB blocks, as `evidence-snapshot.py` already does. On the board, load
addresses and cache maintenance come from each path's prepared session and
its launcher, which the path's admission check validates. Tests run one at a
time, so bundles need no address check against each other.

## Synchronization

The tool is a single process. A snapshot already refuses a source that
changes during its copy and a repository revision that changes during the
snapshot. Two concurrent runs cannot share an output directory, because
creation is exclusive. On the board, the multicore behavior of the OS image
is the subject of ranks 7 and 8 and of the primary agent's scheduler work.
The tool adds no board-side synchronization.

## Guard against board contact

At startup the tool installs a Python audit hook (`sys.addaudithook`), which
cannot be removed for the life of the process. The hook raises on:

- an `open` or `os.open` of `/dev/bus/usb`, `/sys/bus/usb` or
  `/tmp/sunstoneos-a733-*.lock`;
- an import of the `usb` package and a `ctypes.dlopen` of libusb;
- a `subprocess.Popen` or `os.exec*` of `sunxi-fel`, `tools/usb-access.py`,
  `tools/run-*.py` or `tools/board-test-queue.py`.

The preparation modules load supervised-check libraries that also contain
USB code. They call that code only from functions that preparation does not
reach. The hook turns any such call into a refusal before any device file is
opened, and the refused bundle is left without a manifest.

## Reuse and removal

The tool reuses each path's `prepare()` and `validate_bundle()`, its
supervised check's source list, `tools/evidence-snapshot.py` with `--reuse`,
the queue's submission schema and the plan's recovery classes. It adds no
runner, queue, scheduler, recovery tracker or status record. The plan's
ranked table stops being a hand-maintained second copy of the bundle order.

## Budgets and measurement

`checks/board-bundles.py` is a pure-host check. It runs the tool against
fixture preparation modules that write small files, and it also loads the
real table. No baseline below was measured before this design. The first
three rows are new metrics for a tool that does not exist yet.

| Metric | Baseline | Budget | Measurement |
| --- | --- | --- | --- |
| Device opens, USB imports and board tool launches during preparation | Not guarded in any per-path preparation | Zero; each forbidden fixture action is refused and leaves no manifest | Audit-hook event count in the check, with one mutation fixture per forbidden action |
| Bytes copied when a bundle is frozen again with unchanged inputs | 548 KiB for the eight-core launch bundle, by `du` | Zero; every row records `linked_from` | Sum of manifest rows without `linked_from` |
| Reads of a bundle file by the tool outside `validate_bundle()` and the snapshot | Not applicable | Zero | `open` audit events on bundle paths during the submission step |
| Builds started by the tool | Not applicable | Zero; the only subprocesses are the path's own launcher steps and the snapshot | `subprocess.Popen` audit events checked against an allowlist |
| Native USB images ranked before rank 9 | Zero in the current plan | Zero; each Native USB image re-enumerates once per session | Rank order in the generated table, checked against each row's class |
| Summed timeouts of the new rows | Not applicable | At most 1,800 s, each row at most the queue limit of 3,600 s | Sum of `timeout_seconds` from the table |
| Submissions that fail the queue schema | Not applicable | Zero | The check reads the queue's field set and recovery names from the source of `tools/board-test-queue.py` with `ast`, without importing it, and compares each submission |

Each row's expected result also repeats the path's own host-modeled
counters, for example GPU client wake-ups per job, where the image already
reports them in its evidence block. Physical timing runs only in later
sessions under [docs/validation.md](../validation.md).

## Left out

- **Board contact.** The tool does not submit to the queue, claim a test,
  run a runner or read board state. The coordinator holds the queue token
  and the board lock.
- **CPU startup and multicore bundles.** A76 bring-up, CPU power states and
  the remaining multicore steps are the primary agent's. That agent adds
  their rows in the same table format; this package adds none.
- **Excluded tests.** Tests that need a power cycle, an accessory, a voltage
  or clock change, or an intentional hang get no bundle, because the board
  cannot be reconnected today.
- **Recovery-state tracking.** Pending recovery records and retained
  sessions belong to the queue and the coordinator. A copy in the tool would
  be a second status record.
- **Retries.** A failed bundle is not prepared again unless its inputs
  change; a retry in the queue needs a stated changed condition.
