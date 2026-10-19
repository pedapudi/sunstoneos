# Read-only native eMMC application

The eMMC application identifies one explicitly selected controller and serves
bounded copied reads to a USB client and a second native task. It reuses the
[SD read service](storage-read-application.md), its two fixed clients and its
single-core FreeRTOS composition. The target is `sunstone-a733-emmc-read`;
configure it from `apps/emmc-read/CMakeLists.txt` with the pinned dependencies
used by the SD application.

The application has a distinct 912-byte session, contract and launcher identity.
The admitted session selects SDC2 or SDC3; it never probes another controller
after failure. SDC2 uses PC5 clock, PC6 command and PC10 data with mux 3. SDC3
uses those signals with mux 4. The application reserves PC0, PC1, PC5, PC6,
PC8–11 and PC13–16 and acquires both controller clocks through the existing
platform provider. PC1 reset must already be high and is never driven.

Physical population, chip identity, wiring and controller selection require
reviewed authority bound to the exact image and board instance. A device-tree
node, strap observation or identification timeout does not establish those facts.
The optional eMMC population remains unqualified. Admission requires cold card
history, excluded SDC2/SDC3/NAND/SPIF clients, both interrupts disabled and drained,
source-qualified pin pulls and drives, settled supplies and manual PC 1.8 V
configuration. The existing provider observes DCDC1 and DC1SW2 at 3.3 V and
CLDO5 at 1.8 V. It changes no supply voltage.

The unchanged MMC core identifies eMMC with CMD0 and CMD1, reads CID and CSD,
assigns RCA 1, selects the card and reads EXT_CSD using CMD8. It verifies the
user partition, one-bit legacy mode, addressing and capacity before serving
CMD17 or CMD18 reads. Requests contain at most eight 512-byte sectors. Every
result remains bound to its client, ticket, epoch, generation and deadline.
Completed PIO data is copied into private client storage before another request
can reuse the borrower buffer. Cancellation, expiry or uncertain completion
revokes output and permanently retains resources.

`apps/emmc-read/prepare.py` freezes the native image, actual manifest, session,
launcher and reviewed entry evidence. `apps/emmc-read/capture.py` reuses the
existing bounded storage capture and queue admission. Its software prerequisite
is `emmc-read-supervised-software`. The wrapper includes both common and eMMC
host source identities when checking evidence. Fixture files labeled
`MODEL-NOT-PHYSICAL` are software inputs and cannot establish physical authority.

This composition uses one boot CPU and does not depend on sustained multicore
scheduling. It retains the existing storage lifetime: it parks after failure and
requires removal of every board supply for recovery. No healthy close or ROM
return is qualified. No physical launch is scheduled while manual recovery is
unavailable. Preserve all storage contents.

Focused offline evidence lives under
`~/.cache/sunstoneos/evidence/emmc-native-20261018/`. The native model executes
the linked session snapshot, selected controller construction, eMMC
identification, copied reads from both clients, range rejection and terminal
cancellation. Shared providers, translations, scheduling and device effects are
modeled. Host checks consume the native serializer's actual frames, freeze the
complete bundle and reject missing population, routing, voltage, ownership and
cold-state authority. These checks establish software integration only.
