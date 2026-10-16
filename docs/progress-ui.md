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
classification, unknown-format reporting, cache reuse and invalidation, Git
worktree metadata, unchanged queue bytes, secret omission, concurrent queue
changes, document boundaries, symlink refusal, Host/Origin validation and HTTP
conditional requests. Temporary SQLite files and a loopback HTTP server are
confined to the fixture. No board access occurs.

Backend evidence is retained outside Git under
`~/.cache/sunstoneos/evidence/progress-server-20261016/`. Frontend interaction and
visual evidence are recorded separately by the dashboard integration work.
