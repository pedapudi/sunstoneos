# Scheduler carried-commit audit, 2026-11-01

This audit is for maintainers deciding whether to merge the scheduler branch.
It examines the 21 commits below against integration revision `12f4cc56` and
topology branch tip `5d67c638`. It covers default-build exposure, preservation
of each effect and landing restrictions. It is not approval of the unfinished
partitioned scheduler or a substitute for the final integration gate.

“Default” means the ordinary control kernel with `SUNSTONE_LATENCY=OFF`,
`SUNSTONE_A733_CPU_TOPOLOGY=OFF` and `configSS_PARTITIONED_SCHEDULER=0`.
“Retained” means the inspected effect remains, including subsequent extensions.
“Superseded” means later content replaces part of the original effect.
Landing decisions concern merging the current effect with its dependencies;
they do not recommend replaying old documentation over current status.

Only `d2c8de05` changes unconditional scheduling behavior. The board-description
addition in `b307b961` changes compiled data and structure layout even with the
topology option off; it does not change task placement. The other runtime
changes require a diagnostic, explicit application selection or private fork.

| Commit and change | Default option-OFF effect | Effect at integration revision | Safe to carry into bringup? |
| --- | --- | --- | --- |
| `d2c8de05`: bound tick-time ready-list traversal | **Yes:** lists longer than sixteen entries conservatively yield every core running that priority. Shorter lists retain affinity filtering. | Retained unchanged in executable patch hunks; `99aa7330` only changes the explanatory comment. The linked tick checker is byte-identical. | Yes, with named host evidence below and the final gate. This changes the control kernel and must remain visible in review. |
| `99aa7330`: timing limits and sleep protocol | No executable change; the patch edit is a comment. | Comment unchanged. Design prose extended by later recognition and dispatch corrections; status superseded by private-fork progress. | Yes as merged history; preserve current status instead of restoring this snapshot. |
| `d2b0661c`: paired recorder-cost diagnostic | No ordinary runtime change; the new branch requires `SUNSTONE_APPLICATION=latency-cost`. | Diagnostic sources unchanged except the zero-tick expectation repair. Application selection remains present beside later capacity selection. | Yes as an optional diagnostic; it supplies no default-kernel timing guarantee. |
| `e5f526e4`: accept adjacent equal counter reads | No; diagnostic expectation only. | Expectation file unchanged. | Yes. |
| `3a9e4db0`: scheduler research and sources | No; documentation only. | Both files unchanged. | Yes; research conclusions are not implementation qualification. |
| `a1474e04`: count recorder IRQ-tail instructions | No; host checker only. | Checker unchanged. | Yes. |
| `a226132d`: read-only DMA authority control | No ordinary image change; CMake adds an `EXCLUDE_FROM_ALL` probe target. | All seven touched files unchanged, including explicit target selection. | Yes as an optional observation probe; it does not establish DMA drain or writer exclusion. |
| `84c6ff0c`: partitioned scheduler design | No; documentation only. | Review record unchanged. Design superseded in part by head publication, retirement barriers, routing epochs and complete-fork ownership rules. | Yes as history with the current design retained. Original review does not approve later live integration. |
| `b307b961`: topology and weighted ranges | **Yes, data/layout only:** adds the board CPU-policy pointer and all-core policy object. Helpers are not connected to default scheduling. | Board files and checker launcher unchanged. Topology implementation retained with SMT, reserved-field and energy validation extensions. | Yes with current dependent headers and source selection; do not interpret the policy object as enabling A76 scheduling by default. |
| `78166181`: SMT identity and reserved range field | No scheduling effect; topology helper and tests only. | Identity checks and reserved-field initialization retained inside later energy-aware topology source. | Yes with topology dependencies. |
| `e11e42f8`: exhaustive wake/gate/lifetime models | No; models and host checker only. | TLA configuration, specification and source record unchanged. Explorer and checker extended with later negative controls and routing models. | Yes; abstract interleavings do not establish concurrent native correctness. |
| `2bb69689`: optional energy costs | No scheduling effect; topology data and validation only. | Topology implementation, board topology and C fixture unchanged. Header only gains snapshot metadata later. | Yes; energy costs remain optional and A733 capacity remains uncalibrated. |
| `d1c8317b`: phase/core quantiles and exact wake records | No default runtime effect; recorder requires latency instrumentation and workloads require diagnostic selection. | Histograms, exact samples and phase reporting retained. Later attribution enlarges the result layout and changes mask/lock recorder interfaces. | Carry the foundation, but current latency-enabled integration must pass the repair and gate described below. Do not restore the original result layout. |
| `0f08a8bd`: observe actual payload in negative model | No; model only. | Payload observer remains in the extended explorer, including the ungated observation negative control. | Yes. |
| `0525b3db`: corrected recognition evidence | No; documentation only. | Recognition correction and bounded evidence retained; later source-pin and private-fork status supersede parts of the prose. | Yes as history; retain current evidence limits and application pins. |
| `f6705937`: publish phase closure before cutoff | No default runtime effect; latency recorder only. | Closure synchronization retained. Later site attribution changes other recorder paths. Phase-close checker unchanged. | Carry the fix; current latency-enabled integration still requires the later recorder repair below. |
| `25ea7f84`: owner queues and remote activation | No; helper is outside the control scheduler. | Ready FIFO and descriptor foundations retained. Original wake publication and retirement are superseded by head signaling and final-release ordering repairs. | Yes only with the current repairs and disabled live fork; do not replay the original helper as a complete implementation. |
| `d888faff`: one-CAS notification and FIFO wait gate | No; private helper only. | Notification implementation and launcher unchanged. Header gains an ordering comment; fixture gains callback cases. | Yes as a private component; this does not prove live FreeRTOS API coverage. |
| `d3b23ae9`: reduce inbox rescanning | No; owner helper only. | Bounded detached-word scanning retained. Later head publication and all-pending dispatch recheck supersede the original signaling decision. Notification header and fixture unchanged. | Yes with current publication repairs and disabled live fork. |
| `c90dabbf`: record hybrid-dispatch blocker | No; documentation only. | Realtime design unchanged; status superseded by complete private-fork conversion work. | Yes as history; do not replace current status with its earlier blocker inventory. |
| `d6a50b42`: baseline and paused state | No; documentation only. | Baseline and qualification limits retained. Resume instructions and unmerged-work inventory superseded by later adapter progress. | Yes as history; do not restore its stale current-status instructions. |

## Host evidence for the default scheduling change

Evidence paths in this section are beneath
`~/.cache/sunstoneos/build/scheduler-rt-1101/`.
The cached `timeslice-after.log` records a successful execution of
`checks/scheduler-timeslice.py` against
`control-after/sunstone-a733-os-latency.elf`.
The corresponding `timeslice-after.json` names image SHA-256
`21c8564d781a1b62614e4201e0f92ad01d37a0cc72c2c7945998be9e09dc466c`.
The check executes linked `xTaskIncrementTick` instructions in Unicorn,
substituting core identity and SGI publication.

All ten cases pass. At the sixteen-entry limit it reads sixteen task owners.
Lists with seventeen and 1,024 entries both read zero task owners, execute
354 modeled instructions and request the conservative yield mask `255`.
Other cases check affinity exclusions, multiple priorities, the final eligible
affinity and idle-only execution. These are instruction-model measurements,
not physical execution times. The production patch differs from that tested
bounded traversal only in its subsequent explanatory comment.

The cached control regression at `0d0d831c` also records passing kernel and
eight-core soak checks in
`integration/gate-scheduler-b-round2/check-kernel.log` and
`integration/gate-scheduler-b-round2/check-soak8.log`.
The former covers timer preemption, queue ordering, blocking wakeup and FP
context. The latter covers an eight-core queue ring, mutex exclusion, migration
with FP context and preemption on every core.
These results support the retained control behavior; they do not validate
changes introduced after those test inputs.

The recorder foundations have separate passing host records in
`research/review-evidence/percentile-recorder-run.log` and
`partitioned/percentile-build-2/phase-close-recorder.log`.
Those results cover the earlier recorder, including phase quantiles and freeze.
They cannot cover the later attribution merge defect below.

## Landing restrictions

No one of these 21 commits needs permanent exclusion from the current disabled
control series. Preserve the later owner wake/retirement repairs and current
status records when merging. Replaying the earlier owner implementation or
status snapshots would discard necessary corrections.

**Do not land the complete `12f4cc56` integration unchanged.** Its
`runtime/latency.c`, in `ss_latency_lock_acquired`, repeats the acquisition
counter block and declares `unsigned p` twice in one scope. A latency-enabled
build therefore cannot compile that function. Removing only the duplicate
declaration would still double-count acquisitions and retain phase accounting
before the contention cutoff. The defect is a later integration change outside
the 21 audited commits; the coordinator must repair the duplicated block and
check the latency-enabled recorder before landing the complete integration.
The default option-OFF build does not compile this recorder into ordinary images.

Keep the partitioned scheduler and topology integration disabled until their
separate review and configured host/native gates pass. The cached status history
explicitly limits the current host fixtures to private components. No carried
commit establishes physical partitioned scheduling, calibrated heterogeneous
capacity, improved board timing or completion of the accepted OS objective.
