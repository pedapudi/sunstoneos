# Evidence records and status files

Each board result is one line of machine-readable JSON, and each status file is a short summary. A worker records a result with one command and does not write prose for it.

## Records

`docs/evidence/SUBSYSTEM.jsonl` holds one JSON object per line for the status file `docs/status/SUBSYSTEM.md`. [The schema](evidence/schema.json) defines the fields, and `tools/evidence.py validate` checks them:

| Field | Content |
| --- | --- |
| `id`, `entry_id`, `port` | The queue entry ID, which doubles as the record ID, and the board port. |
| `image_sha256`, `git_revision` | The image hash and the revision that built it, copied from `result.json`. |
| `hypothesis` | What the run tests, copied from the entry. |
| `outcome`, `verdict` | The runner outcome from `result.json`, and whether the hypothesis held: `passed`, `failed` or `inconclusive`. |
| `measurements` | Named numbers with units, for example `{"name": "rtt_p50", "value": 104.458, "unit": "µs"}`. Names are lowercase snake case. |
| `conditions` | Clocks, voltages, inputs and other settings, as named strings. |
| `evidence` | Paths to the run directory and other artifacts. |
| `note` | Optional. One or two sentences for a fact that no field holds. |

A record states conditions, measurements, outcome and links. The conclusion belongs in the status file. A result that has no queue entry, such as an older session cited by path, stays in the historical log of its status file until it is run again through the queue.

## Recording a result

```sh
tools/evidence.py add ENTRY_ID --subsystem gpu \
  --measure rtt_p50=104.458µs --measure jobs_per_s=9506jobs/s \
  --condition cpu_b=2002MHz --verdict passed
tools/evidence.py sync        # regenerate the table in each status file
tools/evidence.py validate    # schema, queue agreement, claims
```

`add` reads `entry.json` and `result.json` under `~/.cache/sunstoneos/board-queue/` for the entry ID, port, hash, revision, hypothesis, outcome, date and run duration. Pass `--measure NAME=VALUE[UNIT]` for each key number, `--condition NAME=VALUE`, `--evidence PATH`, and `--verdict` when the default (`passed` for a normal return, `failed` for other outcomes, `inconclusive` for a refusal before launch) is wrong. `--replace` overwrites an existing record.

## Status files

A status file holds, in this order:

1. The title, and `**Status: implemented.**` or `**Status: blocked.**`.
2. An optional paragraph of at most six lines on what is implemented, and an optional `**Software evidence:** host, emulator, source.` line.
3. One `**Board-qualified scope:**` paragraph of at most about ten lines: the inputs, operation and cleanup that a board result qualifies. Write exactly `None.` when no board result qualifies the subsystem.
4. `## Open work`: a numbered list in priority order, one or two lines each.
5. `## Evidence`: a pointer to the records, a link to the historical log, and the table between the `evidence-table` markers, which `tools/evidence.py sync` generates.

Sections that other documents link to keep their own scope paragraph. Do not write queue entry IDs, hashes, image README paths or measured tables in a status file; the table and the records carry them. A status file may cite an entry ID only if a record exists for it, and `checks/progress.py` fails on a cited entry without a record or on a stale table. The layout of the prose is not checked beyond what [the progress view](progress-ui.md) needs: the title, the status line and the scope paragraph.

## History

When a status file changes, move text that no longer describes the current state to `docs/history/status-SUBSYSTEM-YYYYMMDD.md` in the same commit. The log keeps its relative links valid because `docs/history/` has the same depth as `docs/status/`.

## Migrating a prose status file

`tools/evidence.py migrate docs/status/NAME.md` moves the whole file to the historical log, creates a record for every queue entry it cites, and writes the short format with an open-work list extracted from its sentences. Then rewrite the open work in priority order, restore any sections other documents link to, add the `Software evidence` line if the progress view lists host evidence for the subsystem, and run `tools/evidence.py sync` and `checks/progress.py`.
