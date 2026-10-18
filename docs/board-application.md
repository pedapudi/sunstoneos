# Native board application

The board application composes the actual shared resource providers, USB0 device
output, independently scheduled Type-C service and power tasks, and the USB2
read-only host. It reserves private non-cacheable DMA memory before scheduling.
Its first client reads logical sector zero once and publishes all 512 copied
bytes only after data and command-status completion. Physical operation remains
unqualified.

The image derives from the cached eight-core maintenance image. It reuses the
cached boot, continuation, privilege routing, fatal hook, watchdog readback and
native USB0 provider initialization. Storage grants, maintenance workers and
storage protocol entry points are excluded. The application retains one shared
counter, lock service, clock registry, pin manager, rail ledger, PMIC and power
manager. The native radio coordinator uses the same providers, plus one retained CE
source and common pool mutex. Camera startup can use these providers later.

## Immutable startup inputs

`a733_board_session` defines the fixed session at `0x41f00000`. The cached boot
contract uses the board-specific contract magic and binds the complete session
SHA256 in its final four reserved words. Its first two reserved words identify
the session address and size. Epoch, generation, lease, build identity and
absolute watchdog deadlines must agree. The session contains no pointers or
storage authority.

The caller supplies reviewed wiring, memory, recovery and secure SID evidence,
exact provider owners, power profiles, expected pin configurations, Type-C
source limits, comparator bound, FUSB302 identity and controller revision.
Nonzero evidence identities refer to externally qualified facts; creating a
session cannot qualify hardware. There is no usable default session or launcher
that invents those facts.

Primary EL3 reads the four source-defined SID words under explicit session
secure ownership while translation and caches remain disabled. It publishes the
immutable typed identity before starting secondary cores. Its read-only access
does not supply wireless entropy.

The image uses the existing compact cached mapping profile with a two MiB
non-cacheable pool at `0x41c00000`. The USB2 constructor validates translations
and permanently reserves 128 KiB from the actual allocator before concurrency.
The complete controller object graph and private DMA ledger remain retained on
failure. Independent recovery and absence of remote aliases remain admission
requirements.

## Task ownership and copied output

Separate tasks own PMIC service, USB0 device output, Type-C I2C service, Type-C
GPIO power supervision and USB2 host progress. The admitted radio workload adds
its owner task, copied TCP client and watchdog supervisor. Only the USB2 task accesses its
TinyUSB host stack. Host failure revokes Type-C attachment but leaves both
Type-C tasks scheduled for bounded shutdown. Type-C tasks run at higher priority
and delay one tick between calls; deployment requires the existing one
millisecond scheduler tick and adequate scheduling margin.

The device and host each keep their original TinyUSB configuration and logical
port zero. The host object compilation renames the common TinyUSB exports and
time API, so their singleton state and owner-specific timing remain separate.
The actual component sources are unchanged.

USB0 uses the existing native provider and patched programmed-I/O DCD. A host
sends one zero byte to the vendor interface. The 72-byte response header contains
six little-endian 32-bit words: magic `0x52414f42`, version one, USB state, sector
length, radio state and verified radio-byte count. Two little-endian 64-bit
values identify the session epoch and generation; the final 32 bytes identify
the admitted radio profile SHA256.

USB state zero means pending, one means a completed copied sector, and two means
retained failure. Only USB state one carries a 512-byte payload after the header.
Radio state zero means disabled, one means pending, two means a full response
comparison followed by successful TCP CLOSE, and three means failure. Only
radio state two carries a nonzero verified-byte count. The host observer
`apps/board/observe.py` validates these states, lengths and identities against the
admitted session and profile before saving the copied sector. Successful sector
bytes remain immutable until the session expires.

The immutable workload selector chooses USB only or USB with native radio.
USB-only execution uses one externally armed watchdog window. Radio firmware
contains 391,000 bytes, requiring at least 7.82 seconds at the initial one-bit
400 kHz SDIO rate before protocol overhead. The four-second watchdog therefore
requires renewal during radio startup.

The radio supervisor uses the existing hardware reload mechanism. Only an
increased count of successfully completed nonempty SDIO transactions earns a
reload. A coherent publication supplies the current absolute operation deadline;
expired operations, failures and unchanged counts never earn one. Each renewal
retains the fixed session deadline and externally qualified reset margin. An
ordinary radio failure disables further renewal and requests Type-C shutdown.
Scheduler activity, USB polls and empty status reads earn no renewal.

Type-C shutdown begins ten milliseconds before provider admission expires,
leaving time for the existing bounded GPIO shutdown. Expiry closes provider
admission; independent recovery owns faults that stop processors or stall the
interconnect.

## Validation and qualification limits

The complete native image links the actual shared providers, CE and radio
startup, authenticated TCP/RSN sources and both TinyUSB roles. The focused runner
`checks/board-application.py` passed nine AArch64 instruction cases. Client cases
cover continued polling after a copied sector, retained host failure and
attachment loss after a copy. Watchdog cases cover refusal without progress,
renewal after a completed transfer and expired-operation refusal. Output cases
cover completed radio, failed radio and a pending sector. A separate admission executable passed with address and
undefined-behavior sanitizers. The host observer accepted matching output and
refused a changed session identity.

The client and watchdog cases model provider returns; the output cases execute
the actual frame code with modeled endpoint I/O. Existing Type-C and USB2
component evidence supplies their recorded protocol checks. Full native startup,
electrical operation, independent recovery and a physical transfer remain
untested.

Evidence is preserved under
`~/.cache/sunstoneos/evidence/board-application-20261017/batch1/`. It retains source
snapshots, authenticated dependency archives, generated boot/port sources,
compiled dependency bytes and hashes, command logs, the native ELF and every
executed fixture. Failed preparation logs remain separate. The runner resumes
unfinished steps and preserves processor state if an instruction allowance is
exhausted. Successful instruction cases were not repeated after the final
static symbol assertion was corrected for an unused function removed by the
linker.

Physical execution requires qualified boot, memory and power admission,
independent watchdog recovery, the populated switch identity, a suitable
high-speed storage device on TYPEC24 and observable USB0 output. Radio additionally requires the [native radio profile](board-radio.md), matching
read-only firmware spans, qualified CE raw-source characterization and startup
for the actual silicon and health-enable epoch, an access point and a fixed TCP
peer. Missing qualification refuses construction. No board operation, reset or
persistent write is authorized by this image's existence.
