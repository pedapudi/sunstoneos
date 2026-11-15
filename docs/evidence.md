# Evidence records and status files

Each board run is one line of machine-readable JSON. Status and design files describe the present state and point at the records. A worker records a run with one command or one record and does not append results to a design or status file.

## Records

`docs/evidence/NAME.jsonl` holds one JSON object per line. NAME is the stem of the status file `docs/status/NAME.md`, or of the design file that the run tests. [The schema](evidence/schema.json) defines the fields, and `tools/evidence.py validate` checks them:

| Field | Content |
| --- | --- |
| `id`, `entry_id`, `port` | The queue entry ID, which doubles as the record ID, and the board port. A result without a queue entry has a short lowercase slug as its `id` and no `entry_id`. |
| `date` | The date of the run. |
| `image_sha256`, `git_revision` | The image hash and the revision of the source the image ran from (the `--host-tree` it was prepared from, else the board-queue checkout), copied from `result.json`; the daemon's own revision is `daemon_git_revision` there. |
| `image` | The image name or build target, for a run whose hash is not recorded. |
| `design` | The path of the design or status document that describes the feature the run tested. |
| `hypothesis` | What the run tests, copied from the entry. |
| `prediction` | What the run was expected to show, written before the run: the expected figures and the reasoning from the reference. |
| `reference` | The `docs/` heading (`PATH#ANCHOR`) that holds the vendor sequence and the diff the run tests, copied from the entry; `none: REASON` for a coordinator exemption. Absent for entries queued before the flag. |
| `outcome`, `verdict` | The runner outcome from `result.json` (`not-run` for a prediction never run or a host-only result), and whether the hypothesis held: `passed`, `failed` or `inconclusive`. |
| `measurements` | Named numbers with units, for example `{"name": "rtt_p50", "value": 104.458, "unit": "µs"}`. Names are lowercase snake case. |
| `conditions` | Clocks, voltages, inputs and other settings, as named strings. `tools/evidence.py add` fills `kind` and `dram_mode` from the entry and the result. |
| `conclusion` | What the result establishes about the prediction and the design. Required when a prediction is present. |
| `narrative` | The run description, superseded attempts and any figure that no measurement field holds. Required when a record has neither a queue entry nor measurements. |
| `evidence` | Paths to the run directory and other artifacts. |
| `note` | Optional. One or two sentences for a fact that no field holds. |

A board run's prediction and result go in an evidence record, one record per run, in `docs/evidence/<topic>.jsonl`. Write the prediction into the record before the run and add the measurements and conclusion after it. The design or status file keeps only the present-state facts and cites the record by file and ID.

## Records hold no device address

Records are committed and published, while radio scan and association results stay in the board queue on this machine. A record therefore holds no BSSID or other MAC address, and no frequency and RSSI of a single access point, since that pair picks out the access point that the user's credential matched in the queue's scan list. Counts, ranges and an RSSI without a channel frequency are allowed.

`tools/evidence.py` applies the rule when it writes a record and when it validates the committed records:

- **Addresses.** A value written as six colon- or hyphen-separated byte pairs, or as three dot-separated groups of four hex digits, is refused unless it is a protocol constant in `PROTOCOL_ADDRESSES`: the unset and broadcast addresses, the IEEE 802.1X PAE group address and the LLC/SNAP header. A measurement or condition named for a BSSID is refused.
- **Signal pairs.** A sentence with exactly one Wi-Fi channel frequency and one dBm figure is refused; a range counts both of its ends. So are measurements and conditions that hold an RSSI beside a channel frequency.

`checks/evidence-addresses.py` covers both refusals and the allowed constants, and confirms that the committed records pass.

## Recording a result

```sh
tools/evidence.py add ENTRY_ID --subsystem gpu \
  --measure rtt_p50=104.458µs --measure jobs_per_s=9506jobs/s \
  --condition cpu_b=2002MHz --verdict passed \
  --prediction 'p50 near 104 µs: ...' --conclusion 'The measured p50 matches ...'
tools/evidence.py sync        # regenerate the table in each status file
tools/evidence.py validate    # schema, queue agreement, claims
```

`add` reads `entry.json` and `result.json` under `~/.cache/sunstoneos/board-queue/` for the entry ID, port, hash, revision, hypothesis, outcome, date and run duration. Pass `--measure NAME=VALUE[UNIT]` for each key number, `--condition NAME=VALUE`, `--evidence PATH`, `--design PATH`, `--prediction` and `--conclusion` text, and `--verdict` when the default (`passed` for a normal return, `failed` for other outcomes, `inconclusive` for a refusal before launch) is wrong. `--replace` overwrites an existing record.

## Status files

A status file holds, in this order:

1. The title, and `**Status: implemented.**` or `**Status: blocked.**`.
2. An optional paragraph of at most six lines on what is implemented, and an optional `**Software evidence:** host, emulator, source.` line.
3. One `**Board-qualified scope:**` paragraph of at most about ten lines: the inputs, operation and cleanup that a board result qualifies. Write exactly `None.` when no board result qualifies the subsystem.
4. A table of capabilities with their status, key figure and a pointer to the evidence record or design.
5. `## Open work`: a numbered list in priority order, one or two lines each.
6. `## Evidence`: a pointer to the records and the table between the `evidence-table` markers, which `tools/evidence.py sync` generates.

A status file holds only the present state. Conclusions, predictions and run narratives live in the records. Sections that other documents link to keep their own scope paragraph. Do not write queue entry IDs, hashes, image README paths or measured tables in a status file; the table and the records carry them. A status file may cite an entry ID only if a record exists for it, and `checks/progress.py` fails on a cited entry without a record or on a stale table. The layout of the prose is not checked beyond what [the progress view](progress-ui.md) needs: the title, the status line and the scope paragraph.
