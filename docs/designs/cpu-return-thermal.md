# Capture thermal readings after native recognition returns

The returning CPU image retains its first recognition's thermal report in RAM.
Its host previously read only recognition text and CPU shutdown results. USB
operation 7 exposes the thermal report in ordinary OS images, but the returning
profile completes without starting USB. Physical recognition therefore lacked
captured sensor readings across its compute interval.

The host reads the existing thermal window and reading storage after successful
ROM return, before restoring uploaded RAM. The exact image's DWARF identifies
both objects in the thermal producer's compilation unit and supplies member
offsets. The ELF symbol table independently confirms their addresses and sizes.
Ambiguous definitions, missing debug information and incompatible geometry fail
before launch. No native code, recognition arithmetic or return ABI changes.

The window contains an immutable header, its first row index and a publication
flag. Its header fixes the row count when recognition ends. Later sensor samples
may append rows; capture uses the published count rather than the later storage
count. The existing decoder checks session identity, bounds and row ordering.
Its completeness classification remains separate from successful ROM return and
RAM restoration. Incomplete thermal evidence does not establish qualification.

The host reads 33,816 additional bytes in two bounded ROM reads. It adds no
native allocation, sensor access, copy, synchronization or recognition latency.
One existing report decoder handles both USB and returned-RAM capture.

The focused check executes the linked native record emitter and recognition
window publication in each frozen CPU and GPU ELF. Time, board identity and
policy values are modeled. It checks decoding, first-window immutability and
malformed captures. These checks establish the native writer/host reader
contract; physical sensor operation and sustained thermal control need board
evidence.

The next CPU case uses one longer existing recording with eight workers and a
matching host reference. Acceptance requires parity, complete thermal evidence,
all secondary CPUs OFF, normal return and verified boot-rail restoration. A
cool run establishes sampled operation across that finite load interval; it does
not establish a thermal-trip transition or equilibrium under sustained load.
