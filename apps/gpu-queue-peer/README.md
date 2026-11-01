# A second trusted GPU application

This vector-add application joins the owner application's GPU context. Its
immutable manifest and entry run as a separate admitted task with a GPU-only
grant. The owner explicitly links `GPU_QUEUE_PEER_COMPOSITION` to consent to the
shared program and regions. Admission consumes one of the owner's granted
worker cores and checks that both manifests describe the same regions.

The peer reads and writes at byte offset 1024 in the two shared regions. Its
job has sequence one and verifies all 65 sums before publishing completion.
The primary lifecycle application uses the beginning of those regions and
waits for the peer before closing the device. Each task owns its submission
slot and completion queue. Global create, open, close, recovery and session
completion belong to the owner.

The applications are trusted and share the program and GPU address space.
Independent write ranges prevent a data race in this composition. The fixed
peer manifest expects the primary's two 4096-byte regions at physical addresses
`0x44000000` and `0x44200000`; admission refuses a different layout.

This is an implementation and software-test consumer. GPU execution, concurrent
completion and fault recovery need physical qualification on the A733 board.
