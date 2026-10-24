# Reading one block from a direct NVMe endpoint

The supervised board image `sunstone-a733-pcie-nvme` reads one 4096-byte block
from an NVMe controller on the direct x1 PCIe port, shuts the controller down,
stops the port and reports a copied result through USB0. It links the
[general function client and read-only NVMe driver](pcie.md) with the same
shared clock, pin, rail, PMIC and register-mutex providers as the other board
applications. The image is **implemented** and passes linked-instruction and
host checks against a modeled controller. No NVMe device has been attached,
and physical operation is unqualified.

## One task runs the read and the orderly stop

The image derives from the supervised board image through
[a733-board-application.cmake](../cmake/a733-board-application.cmake) and
compiles the PCIe units with `SUNSTONE_PCIE_FUNCTION`, which enables the
function client's exact native write rules. One privileged task owns the port
and runs these stages in [run.c](../apps/pcie-nvme/run.c):

1. Bring the port to READY at 2.5 GT/s, as the configuration-only PCIe
   application does.
2. Initialize the NVMe driver for the admitted vendor, device, function and
   namespace, with polled completion. INTx stays disabled in the command
   register and MSI-X stays off, so the image links no GIC route or handler.
3. Poll the driver through function scan, BAR assignment, controller enable,
   identify and I/O queue creation.
4. Submit one read of the admitted LBA, wait for its phase-tagged completion
   and copy the 4096 bytes into private application memory.
5. Delete the I/O queues, request a normal controller shutdown, clear bus
   mastering after shutdown completes, stop the function and stop the port
   with PERST held.
6. Publish the result only after the port reports STOPPED.

The driver places BARs from `0x22000000`. The application checks that the
claimed function's whole region-one window has a Device-nGnRnE mapping after
BAR assignment and before the first BAR access, and fails without touching
the BARs otherwise.

DMA uses six pages of the image's non-cacheable pool at
`0x41c00000..0x41e00000`: the admin and I/O queues, the identify page and the
data page. The CPU reads them through that non-cacheable mapping, so no cache
maintenance is involved. The driver refuses pages that end above 4 GiB, because
the root port's inbound address width is unpublished; the pool lies below
4 GiB, so the refusal does not fire in this image. A disabled inbound translation unit forwards device
addresses unchanged, so the controller can reach any address; the admitted
controller is trusted, as for every PCIe client in this project.

## Every failure retains its resources

Any refusal, timeout, deadline expiry or uncertain result stops the task's
progress. The port's clock and rail leases are retained, and the NVMe queue and
data pages are marked retained in the pool. The application marks them itself
when it detects a failure that the driver has not reported. The controller is never reset after a failure:
a read without a completion, for example, leaves bus mastering set because
nothing proves that the controller has stopped writing. A successful run also
retains the pages and every port lease, because a stopped port cannot restart
within the same boot. Recovery is whole-board power removal.

## The session binds the read target and its authority

The 848-byte session extends the board session at `0x41f00000` with the PCIe
owner and authority, the startup bound, the four PCIe evidence digests and the
inherited PD20–PD23 pin configuration, as for the other PCIe images. It adds:

- vendor and device ID of the claimed function, the function number and the
  namespace ID;
- the LBA to read;
- a bound of 1 ms to 5 s on each NVMe command and on the controller shutdown;
- the SHA-256 of a reviewed NVMe record.

[prepare.py](../apps/pcie-nvme/prepare.py) accepts that record only when it
names the same image, target, vendor, device, function, namespace and LBA. The
record must also give the controller's model, serial number and firmware, and
it must retain its evidence files by hash. It must assert every required fact:
BAR access, a disabled controller at handoff, the NVM command set, permission
to read the namespace, queue DMA, trusted identity forwarding and permission
for a normal shutdown.
Those facts are claims a reviewer makes about real hardware; the host tools
cannot establish them.

## USB0 returns a copied identity record and the data

A host sends one zero byte to the vendor interface. The reply starts with a
128-byte header: little-endian magic `0x4d564e50`, version 1, status and payload
length, the session epoch and generation, the SHA-256 of the session and of the
payload, and 32 zero bytes. Status 0 means pending, 1 a completed result and 2
a retained failure; only status 1 carries a payload. The 4288-byte payload is a
192-byte record followed by the 4096 bytes read:

| Offset | Content |
| ---: | --- |
| 0 | Completion time in microseconds (u64) |
| 8, 12 | Data bytes, 4096; flags, 3 (read copied, orderly stop complete) |
| 16 | Vendor ID, device ID, subsystem vendor ID and link status (u16 each) |
| 24 | NVMe version, namespace count (u32 each) |
| 32 | Namespace size in logical blocks (u64) |
| 40, 44 | LBA shift, maximum transfer size exponent (u32 each) |
| 48 | LBA read (u64) |
| 56, 60 | BAR0 address and size (u32 each) |
| 64, 84 | Serial number (20 bytes), firmware revision (8 bytes) |
| 92 | Namespace ID (u32) |
| 96 | Model (40 bytes) |
| 136, 140 | Class code and revision, function number (u32 each) |
| 144 | Reviewed NVMe record SHA-256 |
| 176 | Zero |

[observe.py](../apps/pcie-nvme/observe.py) checks the header against the
session, the flags, the identity and read target against the session, the
namespace geometry, the BAR0 placement and the record digest. The block
contents cannot be checked on the host because they come from the medium.
[capture.py](../apps/pcie-nvme/capture.py) uses the existing supervised
transport, queue claim and target lock, and saves the payload durably.
The USB product string is `sunstoneos PCIe NVMe read`.

## Build and checks

Configure with `SUNSTONE_BOARD_SUPERVISED=ON` and `SUNSTONE_A733_PCIE_NVME=ON`
and build `sunstone-a733-pcie-nvme`. The manifest profile is
`pcie-nvme-supervised-v1` and the workload is `nvme-read-one-block`.

The checks run in this order; each consumes the previous output:

```sh
checks/pcie-nvme-fixture.py --image IMAGE --output FIXTURE
checks/pcie-nvme-native.py --image IMAGE --session FIXTURE/session-template.bin --output NATIVE
checks/pcie-nvme-host.py --image IMAGE --fixture FIXTURE --frame NATIVE/native-frame.bin --output HOST
```

- The fixture builds a session and admission from the existing PCIe software
  fixture and a modeled NVMe record, through the production validators.
- The native check builds [pcie-nvme-registers.c](../checks/pcie-nvme-registers.c)
  with the NVMe host model to obtain the register state of a READY port with a
  cold NVMe function. It then runs the linked image in Unicorn: the application
  stages, the function client, the NVMe driver, the native write filter and the
  USB0 serializer execute as linked. A modeled controller answers BAR sizing and
  processes queue entries from the non-cacheable pool, and requires bus
  mastering and device-owned pages for every modeled DMA. The nominal case
  issues two identify commands, the queue-count and queue-creation commands, one
  read and the two queue deletions, in that order. It copies the expected bytes,
  calls the port stop once and leaves the pages retained. The decoded USB frame
  matches the payload. At READY the native filter refuses a controller enable
  outside a driver session, an I/O doorbell, a BAR reassignment, a command
  register value with I/O decode, a region-one retarget and a root window
  change, and accepts the current command value. Four failure cases end with
  status 2, retained pages and retained leases. An unmapped BAR window is
  refused before any BAR access or bus mastering. A read without completion and
  a shutdown that never completes both keep bus mastering set and never stop the
  port. An LBA outside the namespace is refused before any read is submitted.
  Removing the window check from the application makes the unmapped-window case
  fail.
- The host check refuses session geometry errors, NVMe records that miss a
  fact, name another LBA or schema or lack a controller identity, and altered
  frames. It captures over a modeled exchange, refuses to retry after a transport
  loss, and builds the launcher and a frozen bundle from the tested image.
- `checks/h264-supervisor-entry.py --caller apps/pcie-nvme/capture.py` stops at
  readiness before any queue or device access and confirms that the supervisor
  accepts the capture bounds.

Port startup and `a733_pcie_stop` are replaced by their results in the native
check; [a733-pcie-trace.py](../checks/a733-pcie-trace.py) covers them in the
host model. Provider status, address translation, the scheduler, the controller
and USB transport are modeled.

## Remaining work

- Physical qualification with a real NVMe device on the FPC connector: link,
  identify, read, shutdown, port stop and DMA visibility.
- INTx or MSI-X completion, which needs GIC routing for INTID 184 or 185.
- Reads of more than one block, and Gen2 link training for this image.
