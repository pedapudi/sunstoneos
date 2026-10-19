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

The server reads [the hardware matrix](hardware.md), [the handoff](handoff.md),
[the mimus record](mimus.md), and [the validation policy](validation.md). It does
not maintain a separate status file. Every hardware inventory row appears,
including rows with no implementation. Stable name aliases associate the
matrix's partial-evidence bullets with their inventory rows. Expanded details
retain the canonical prose and documentation links.

Implementation, evidence, and qualification are separate fields. An implemented
row means that some of its scope has code. A physical result applies only to its
recorded conditions. Host or emulator evidence does not establish physical
operation. The current matrix explicitly declares no qualification evidence for
any row; the dashboard therefore shows zero qualified rows.

Physical-result labels require known explicit statements in the matrix, such as
boot-CPU instruction execution or USB high-speed exchanges. A host DMA test does
not receive that label. Host, emulator, and source labels require affirmative
evidence wording; pending tests do not count. These conservative text rules do
not independently audit a test result. The original scope remains visible for
review.

If the inventory table or qualification declaration changes format, the API
reports an unsupported parser state and unknown qualification totals. It does
not reinterpret missing rows as planned work. Unmatched evidence bullets appear
in parser warnings with their original text so that aliases can be updated.
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
modification time, size, inode and type before rebuilding a snapshot. An unchanged
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

Document paths must name a Markdown file directly within `docs/`. Each path
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
python3 -B checks/progress.py
```

The focused fixture checks inventory coverage, planned rows, host-versus-physical
classification, dependency coverage, cycle detection and unknown-format reporting.
It also checks cache invalidation, Git worktree metadata, unchanged queue bytes,
secret omission, concurrent queue changes, document boundaries, symlink refusal,
Host/Origin validation and HTTP conditional requests. Temporary SQLite files and a loopback HTTP server are
confined to the fixture. No board access occurs.

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
