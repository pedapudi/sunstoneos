# Local development dashboard

The development dashboard displays the canonical hardware inventory, partial
implementation and test evidence, mimus status, next actions, blockers, local Git
activity, and the board-test queue. It runs on the development machine and does
not communicate with the board.

Start it from the repository:

```sh
python3 tools/progress.py
```

Open `http://127.0.0.1:8765`. Stop the server with Ctrl-C. To display another
checkout or select a port and an existing queue:

```sh
python3 tools/progress.py --repository /absolute/path/to/sunstoneos \
  --port 8765 --queue /absolute/path/to/board-tests.sqlite3
```

The default repository is the parent of the script's `tools` directory. The
default queue is `~/.cache/sunstoneos/board-tests.sqlite3`. A missing queue appears
as unavailable; the server never creates or initializes it. Frontend assets
must be present in `tools/progress-ui/` in the displayed repository.

## Canonical documents determine the displayed scope

The server reads [the hardware matrix](hardware.md), its linked subsystem
records in `docs/status/`, [the handoff](handoff.md), and
[the validation policy](validation.md). An external application's source
declaration can also select its evidence document. Every hardware inventory
row appears. Expanded details retain the canonical prose and documentation links.

Each inventory row links to its canonical record through `[State](status/NAME.md)`.
A record starts with a title and a standalone `**Status: implemented.**` or
`**Status: blocked.**` declaration. Its introductory `**Board-qualified scope:**`
paragraph states the physical result, or `None.` when no physical result applies.
A link fragment selects a section with its own scope paragraph and inherits the
record's status. For example, `status/memory-dma.md#dma` can record no physical
DMA result while the same document records DDR initialization.

Implementation, evidence, and qualification are separate fields. An implemented
row means that some of its scope has code. A physical result applies only to its
recorded conditions. Host or emulator evidence does not establish physical
operation. The current matrix states that no row is fully qualified across all
its required functions. The dashboard shows partial physical results, including
GPU compute and NPU inference, while retaining zero fully qualified rows.

Physical-result labels require an explicit, nonempty board-qualified scope other
than `None.` in the selected record or section. Host, emulator, and source labels
require affirmative evidence wording; pending tests do not count. These text
rules do not independently audit a test result. The original scope remains
visible for review. Older matrices without State links retain their existing
prose parser and physical-result anchors.

If the inventory table or qualification declaration changes format, the API
reports an unsupported parser state and unknown qualification totals. It does
not reinterpret missing rows as planned work. A missing or malformed status
record, scope paragraph or selected section produces a visible warning and an
unknown row status. Matrix prose cannot supply a replacement claim for that row.
The ordered next actions and blocker paragraphs are copied from the handoff.

## Dependency graph covers the complete inventory

The **Dependencies** tab displays the implementation graph from
[hardware-dependencies.md](hardware-dependencies.md). Select a function to see
all its prerequisites and direct dependents. Enable **Show the complete graph**
to explore every hardware area and shared function. Arrows point from prerequisite
to dependent; the columns reflect dependency depth, not a required serial schedule.

The detail panel shows the selected function's required scope, current inventory
evidence, direct relationships and physical qualification conditions. It links to
the component record and support matrix. Missing cameras, panels and other
accessories remain visible as qualification requirements.

The graph table contains topology and scope only. Matching inventory IDs obtain
evidence automatically from the hardware matrix; shared-function nodes link to
component evidence without inferring readiness. Update the graph only when a
prerequisite or required scope changes. Normal evidence updates need no graph edits.
The server rejects missing inventory rows, duplicate IDs, unknown prerequisites
and cycles without suppressing the other dashboard views.

## Refreshes perform bounded local reads

The browser refreshes while the page is visible. The server compares file
modification time, size, inode and type before rebuilding a snapshot. Inputs
include every Markdown file directly in `docs/status/`, missing State-link
targets, and the status directory itself. Adding, changing, removing or restoring
a subsystem record invalidates the snapshot. An unchanged
snapshot retains its HTTP ETag, allowing a conditional request to receive an
empty `304` response. No language model, external API, build, benchmark or device
command runs during refresh.

The snapshot's `updated_at` is the newest observed input modification time.
`generated_at` records snapshot construction. The browser separately reports
when it last checked the server. Those times do not establish when a physical
test ran; the canonical evidence documents carry that information.

Git worktree pointers and shared Git directories are supported. The server reads
HEAD, branch references and a bounded tail of the local HEAD reflog. Reflog entries
appear as recent activity because checkout and reset operations can appear
alongside commits. It does not invoke Git or read Git configuration.

Queue reads select only the halt marker, counts and submission summaries.
Coordinator token hashes, evidence paths and full submission records are not
returned. SQLite opens the existing file in read-only immutable mode to avoid
lock, journal or shared-memory writes. A nonempty WAL or rollback journal makes
the queue temporarily unavailable. Changes to the database or journal metadata
during a read also defer the snapshot until a later refresh.

## HTTP access is restricted to local read-only content

The server binds only to IPv4 loopback. It accepts the configured port with a
Host name of `127.0.0.1`, `localhost`, or `[::1]`; an Origin header must match the
request's local origin. It sends no cross-origin access permissions.

The server exposes these read-only endpoints:

| Endpoint | Content |
| --- | --- |
| `/` | Dashboard from `tools/progress-ui/index.html` |
| `/app.js`, `/app.css` | Allowlisted frontend assets |
| `/api/progress` | Cached JSON snapshot, schema version 1 |
| `/api/document?path=docs/hardware.md` | Canonical Markdown as plain text |
| `/api/source?path=docs/hardware.md` | The same document as JSON with path, title and content |

Document paths must name a Markdown file directly within `docs/` or
`docs/status/`. Relative links resolve from the document containing them. Each path
component is opened without following symlinks. Arbitrary files, traversal,
duplicate document parameters, Git files and symlinked assets are refused.
Errors return fixed text without reflecting filesystem paths. Content security
headers restrict scripts, styles, requests and framing to the local application.

There are no write endpoints, user accounts, saved preferences on the server,
credentials, or command-execution endpoints. Browser filtering and expanded
panels do not modify project state.

## Focused verification

Run the backend check with:

```sh
python3 -B checks/progress.py --output /absolute/new/progress-checks
```

The focused fixture checks inventory coverage, planned rows, host-versus-physical
classification, dependency coverage, cycle detection and unknown-format reporting.
It also checks scoped subsystem records, missing and malformed records, nested
document access, relative links, cache invalidation, Git worktree metadata, unchanged queue bytes,
secret omission, concurrent queue changes, document boundaries, symlink refusal,
Host/Origin validation and HTTP conditional requests. Temporary SQLite files and a loopback HTTP server are
confined to fresh fixture directories beneath the required `--output` directory.
The output directory must not exist. Every fixture remains there for inspection
after the check. No board access occurs.

Subsystem-record parser and headless browser checks pass under
`~/.cache/sunstoneos/build/check-selection-1031/dashboard/`. The browser checks
cover relative links in hardware and dependency details, blocked badges and
refresh after a record becomes malformed. That refresh clears the old physical
result and displays an unknown status with its warning.

Backend evidence is retained outside Git under
`~/.cache/sunstoneos/evidence/progress-server-20261016/`. The integrated browser check passes search and filters, expanded-state retention,
source-table/raw views, linked-document navigation, queue/activity views, and
mobile layout. Desktop and mobile screenshots and results are retained in
`~/.cache/sunstoneos/evidence/progress-ui-browser-20261016/`.
The first browser harness used a string-evaluation wait prohibited by the page's
content policy. Locator assertions replaced that wait; only the remaining
interaction checks continued. No application change or hardware test was needed.

Dependency graph verification covers complete inventory coverage, scoped
prerequisites, invalid references, cycles and cache invalidation. Browser checks
cover node selection, complete-graph expansion, component navigation, refresh
retention and mobile overflow. Evidence and screenshots are retained under
`~/.cache/sunstoneos/evidence/progress-dependencies-20261018/`.
