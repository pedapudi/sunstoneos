# Serialize tests per board while development proceeds in parallel

Development workers build and check components in separate worktrees, then submit
physical test proposals through `tools/board-test-queue.py`. One coordinator
reviews proposals and performs board operations. The tool stores metadata in
SQLite and never executes submitted procedures.

Each board has a separate database, shared by worktrees targeting that board.
The original board on port `3-1` uses the default
`~/.cache/sunstoneos/board-tests.sqlite3`. Keep the database on the local host filesystem. SQLite transactions
and a unique index permit only one active claim. A claim remains active after the
claiming command exits, the coordinator disconnects, or the host restarts. There
is no automatic expiry, retry, or claim release.

A queue claim reserves the testing schedule. Every actual device operation must
also hold `/tmp/sunstoneos-a733-<USB-port>.lock` and follow the identity, USB health, reconnect
permission, and recovery rules in [AGENTS.md](../AGENTS.md). Supervised RAM-only
tests may use a reviewed manual-recovery plan. Unattended fault tests still need qualified independent recovery. The failed UFS probe and
intentional watchdog expiry remain deferred. A queue claim does not override an
image's admission requirements or establish physical recovery.

The shared `board_lock()` helper in `tools/usb-access.py` selects the device
through host metadata and locks its physical USB port. Port `3-1`, for example,
uses `/tmp/sunstoneos-a733-3-1.lock` across USB re-enumeration. It checks that the
selected node has not changed before yielding it to the caller. Other ports and
non-A733 mimus boards do not share this lock. The queue schedules SunstoneOS
tests only; it does not reserve unrelated boards.

## Attached boards have independent ownership

| USB port | Card | Queue database | Coordinator token |
| --- | --- | --- | --- |
| `3-1` | Original microSD; preserve contents | `board-tests.sqlite3` | `board-test-coordinator.token` |
| `3-3` | User-reported 32 GB microSD; preserve contents | `board-tests-3-3.sqlite3` | `board-test-coordinator-3-3.token` |

Set `SUNSTONE_USB_PORT=3-1` or `SUNSTONE_USB_PORT=3-3` on each runner command.
This selects a physical port before the existing lock and identity checks.
The original board is reserved for direct multicore investigation; the second
can run independent device work after its own recovery and platform admission.

Queue and token files are under `~/.cache/sunstoneos/`. Use the matching
`--database` and `--token-file` arguments. A failure halts that board's queue;
recovery evidence and admission never transfer between boards. Do not copy a
live device identity, RAM reservation or electrical qualification to the second
board. USB enumeration numbers change; the physical port identifies ownership.

## Initialize once and submit immutable proposals

The coordinator initializes the shared database and retains the token file:

```sh
python3 tools/board-test-queue.py \
  --token-file ~/.cache/sunstoneos/board-test-coordinator.token init
```

Initialization refuses to overwrite either the database or token file. The token
controls coordinator operations through this CLI. Files use owner-only access,
but agents running under the same host user can read them. This mechanism
prevents accidental role confusion; it is not an isolation boundary against an
agent that ignores its assignment. Workers receive the database path and submit
proposals without using the coordinator token.

A proposal is a JSON object with these fields:

| Field | Meaning |
| --- | --- |
| `component` | Hardware function and test purpose. |
| `kind` | `peripheral` for functional tests or `recovery` for recovery qualification. |
| `sources` | Nonempty list of `repository` and full 40-digit Git `revision` records. |
| `artifacts` | Nonempty list of absolute `path` and lowercase `sha256` records for the test inputs. Include the executable image and any required firmware or fixture. |
| `prerequisites` | Distinct names for conditions the coordinator must verify. Peripheral tests must include `independent-watchdog-rom-return` or `supervised-ram-recovery`. |
| `expected_result` | Observable acceptance conditions, including required recovery behavior. |
| `procedure` | Ordered, nonempty list of instructions for one bounded attempt. These strings are never executed by the queue. |
| `timeout_seconds` | Integer from 1 through 3600; the coordinator must enforce this bound through the approved test procedure. |
| `stop_condition` | Conditions requiring immediate cessation, including the first transport failure. |
| `evidence_directory` | Absolute destination for the result, logs, and input identities. |
| `retry_of`, `retry_reason` | Optional pair identifying a failed or blocked proposal and explaining changed conditions that justify another attempt. |

Use committed source revisions and preserve artifacts outside the worktree when
later builds could overwrite them. Submission records identities but does not
read artifacts. The coordinator verifies artifact hashes during review and
again during claim. The coordinator must also establish that the artifacts came
from the recorded sources and that existing host evidence applies.

```sh
python3 tools/board-test-queue.py submit /absolute/path/proposal.json
python3 tools/board-test-queue.py list
python3 tools/board-test-queue.py show SUBMISSION_ID
```

The submission ID is the SHA-256 of the canonical JSON object. Submitting the
same proposal returns the existing ID and state. Submission contents are
immutable. Changes require another proposal; terminal records retain their
results and event history. The list reports states, owners, prerequisites, and
whether a failure has halted claims. `show` includes the complete proposal,
review evidence, and history.

## Review prerequisites and claim one test

The coordinator creates a JSON object mapping every prerequisite name to an
absolute evidence `path` and its `sha256`. For example, a prerequisite named
`host-check` could reference a saved manifest containing the emulator result and
input hashes. The independent-watchdog prerequisite must reference physical
qualification records. A supervised-RAM prerequisite must document the specific board, bounded
RAM-only procedure, storage preservation, demonstrated return path and manual
reconnection plan. It cannot admit unattended fault testing or an intentional
reset into unknown storage software. Existence and hash checks cannot determine
whether evidence proves the condition; that judgment belongs to the coordinator.

```sh
python3 tools/board-test-queue.py \
  --token-file ~/.cache/sunstoneos/board-test-coordinator.token \
  review SUBMISSION_ID /absolute/path/prerequisite-evidence.json \
  --note 'Reviewed input identities, acceptance conditions, recovery, and host evidence.'
python3 tools/board-test-queue.py \
  --token-file ~/.cache/sunstoneos/board-test-coordinator.token \
  claim SUBMISSION_ID --owner root
```

Review moves a pending proposal to ready only when every prerequisite has a
matching evidence record and all recorded hashes match. Claim repeats content
checks and refuses if any other proposal is claimed or a failure has halted the
queue. It selects the requested proposal; it does not reorder submissions or
start a process. The coordinator chooses the next test from its dependencies
and can batch compatible cases in one submitted procedure to avoid extra boots.

A missing prerequisite can remain pending while independent work continues.
The coordinator can instead close the proposal as blocked with a saved note:

```sh
python3 tools/board-test-queue.py \
  --token-file ~/.cache/sunstoneos/board-test-coordinator.token \
  block SUBMISSION_ID --evidence /absolute/path/blocker.json \
  --note 'The required physical recovery arrangement is unavailable.'
```

Blocked is terminal. Resuming that test requires a proposal with `retry_of` and
`retry_reason`, followed by a fresh review. A changed artifact or expired hardware
assumption also requires review before execution, even if the queue still reports
ready. The tool verifies file content, not current electrical or device state.

## Record results and retain ownership through failures

After stopping all test processes and resolving their device access, the
coordinator records a result:

```sh
python3 tools/board-test-queue.py \
  --token-file ~/.cache/sunstoneos/board-test-coordinator.token \
  finish SUBMISSION_ID --result passed \
  --evidence /absolute/path/result-manifest.json \
  --note 'Acceptance conditions and ROM return passed.' --confirm-stopped
```

`finish` accepts `passed`, `failed`, or `blocked`. It hashes the evidence file and
retains the owner and claim history. `--confirm-stopped` asserts that associated
device processes have stopped; elapsed time alone cannot establish that fact.
After a coordinator interruption, inspect actual process handles and the device
lock before finishing the retained claim. A timeout or unavailable process
observation does not justify releasing it.

A failed result halts every subsequent claim. The coordinator must inspect the
failure and document a safe recovery arrangement before clearing that halt:

```sh
python3 tools/board-test-queue.py \
  --token-file ~/.cache/sunstoneos/board-test-coordinator.token \
  resume --evidence /absolute/path/recovery-review.json \
  --note 'Reviewed the failure and established safe conditions for the next attempt.'
```

Resume requires no active claim and records its evidence in the database event
history. It does not change terminal test states, clear USB failure records,
qualify recovery, or perform a reset. Recovery procedures remain subject to the
existing board rules. The coordinator uses `failed` for an attempted device
operation that failed; `blocked` records an unavailable prerequisite or a test
that could not proceed. Never classify a transport failure as blocked to avoid
the queue halt.

## Host evidence covers scheduling and state retention

`python3 checks/board-test-queue.py` uses a temporary database, synthetic artifacts,
and separate host processes. It checks submission deduplication, coordinator
access, prerequisite completeness, artifact changes, and concurrent claims.
It also checks retained ownership after the claiming process exits, explicit
release, failure halt, recovery review, terminal states, and immutable proposals.

The focused check passed on 2026-10-16. Its log and input hashes are retained in
`~/.cache/sunstoneos/evidence/board-test-queue-20261016/`. No physical device or
emulator was accessed. This evidence establishes scheduling behavior; physical
recovery and hardware qualification remain separate requirements.

## Supervised recovery admission check

The focused submission check accepts either documented recovery prerequisite
and rejects missing or duplicate recovery prerequisites. All four cases pass;
no board access occurs. Inputs and results are preserved under
`~/.cache/sunstoneos/evidence/watchdog-deferral-20261017/`. The three physical
watchdog preparation and reset proposals are deferred using the queue's
`blocked` state, with their original procedures and history retained.
