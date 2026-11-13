# Reopen and recover the GPU queue

This application exercises two retained-context operations through the native
GPU queue. Its application control flow and result decoder are host-verified;
physical execution remains unqualified. It uses the vector-add program and source record in
[queue-add](../queue-add/README.md). Every successful job adds 65 unsigned
32-bit elements, checks every output and requires checksum 8,840. Rotating the
vectors between jobs rejects output left by an earlier job.

The default image opens the GPU once during context creation, then completes
ten open/job/close cycles. After initial context creation it holds the open
owner idle for one second, using precise application waits and rechecking the
absolute counter target after every notification. This phase fits the existing
20-second application and five-second progress budgets. The GPU receipt
reports cumulative owner event and deadline wakes, idle notification wakes and
microseconds spent in indefinite notification waits. Their ratio measures
notification wakes during those waits; it does not measure core WFI residency.
The application preserves the context, program,
leases and rising job sequence across the nine explicit reopen calls. A fixed
peer application shares the first cycle, uses separate ranges in the same
manifest regions and completes before the owner closes the GPU.

The recovery image withholds completion observations for the owner's first
normal vector job until its ordinary 200,000-microsecond deadline expires. It
requires that job's `FAULT` record with detail `HANG`, waits for recovery to
restart the GPU, then checks a second job with a new sequence. The peer queues
its own job during the owner's first job and acknowledges accepted submission
within 50 scheduler ticks, before the owner waits for its fault. The peer must
then finish across the recovery.
This test requires the qualification-only
`SUNSTONE_A733_OS_GPU_RECOVERY_TEST` build option. It does not establish
recovery from a GPU that never becomes idle; uncertain idle state retains the
resources and fails the run.

A parked owner submission waits up to 500 scheduler ticks, yielding one tick
between attempts while the session remains live. The same bound applies after
reopen and recovery. Other submission results stop the diagnostic.

The primary application uses input bytes 0–519 and output bytes 0–259. The
peer uses ranges beginning at byte 1,024. The manifest grants one worker for
the peer task, a 20-second application budget and five seconds for return.
The image's existing supervised session and watchdog bound its whole lifetime.
Only the coordinator runs a prepared returning image on a board.

Build with `SUNSTONE_APPLICATION=gpu-lifecycle` and
`SUNSTONE_A733_OS_GPU=ON`.
`SUNSTONE_GPU_LIFECYCLE_PEER=ON` links the fixed peer. The recovery image also
selects `SUNSTONE_A733_OS_GPU_RECOVERY_TEST=ON`. Dependency paths, frozen
session inputs, image hashes and exact coordinator commands belong in the
prepared image package.

The result contains 22 little-endian 32-bit words (88 bytes):

| Word | Field | Ten-cycle image | Recovery image |
| --- | --- | --- | --- |
| 0 | Format magic | `0x314c5047` | `0x314c5047` |
| 1 | Mode | 1 | 2 |
| 2 | Successful opens, including recovery | 10 | 2 |
| 3 | Successful closes | 10 | 1 |
| 4 | Owner's successful jobs | 10 | 1 |
| 5 | Last owner sequence | 10 | 2 |
| 6 | Last owner checksum | 8,840 | 8,840 |
| 7 | Expected failed job result | 0 | `GPU_QUEUE_FAULT` (4) |
| 8 | Expected failed job detail | 0 | `GPU_QUEUE_HANG` (11) |
| 9 | Successful recoveries | 0 | 1 |
| 10 | Successful peer jobs | 1 | 1 |
| 11 | Elements per successful job | 65 | 65 |
| 12–21 | Successful owner job times in microseconds | Ten times | One time, nine zero words |

[host.py](host.py) validates the complete payload, exact lifecycle counts and
completion times below the job deadline. The runner's GPU receipt and retained
breadcrumbs establish separate retirement and failure evidence. A decoded
payload alone does not qualify physical execution or safe return.
