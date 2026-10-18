# Repeated thermal and GPADC monitoring

The supervised native FreeRTOS image combines five thermal sensors and passive
GPADC channels 0–3. Two permanent owner tasks service the actual controller APIs.
Applications obtain copied latest observations through
`apps/monitor/service.h`; USB exposes the same snapshots. Physical operation and
configured-disable behavior are unqualified.

The ADC owner first borrows the shared CLDO3 rail and reads its programmed
1.8 V profile through the actual PMIC provider. This does not measure the supply.
The sealed admission must independently establish safe, stable analog power,
internal reference behavior, input limits and source impedance. GPADC reports
raw codes, including zero. It does not infer calibrated voltage or strap identity.
The existing board topology audit restricts this mode to four selection dividers.
No external drive, pad change, reference change or strap modification is allowed.

A separate permanent bootstrap clock token excludes every GPADC24M consumer.
Under the shared register mutex, the ADC owner verifies oscillator, reference,
APB1 and optional PERI0 profiles. It either adopts the exact supported 24 MHz
parent or configures the admitted cold parent once. Atomic stable publication
replaces the exclusive token without a release/reacquire interval. The
replacement remains owned permanently. Only then do the ADC and thermal owners
acquire their separate local-exclusive tokens with shared GPADC24M authority.
Neither consumer can write the parent. Local startup does not assert reset.

GIC INTIDs 70 and 71 route to the primary CPU as level-high Group1NS interrupts.
Each bounded ISR masks its interrupt and sets an atomic pending flag. It does
not access converter status or the shared mutex. The owner exchanges pending
before service and rearms only while sampling remains admitted. A notification
arriving during service survives until the next owner iteration. Periodic
service also collects latest observations when no interrupt arrives.

The service continues collecting fresh complete per-device sets until stop or
failure. Each numbered 192-byte snapshot contains independent thermal and ADC
set counters, per-channel observation times, thermal calibration and temperatures,
raw codes, and accumulated observed thermal event masks. The sets are
asynchronous latest values. Neither synchronous conversion nor lossless event
counting is claimed. Event masks are staged until the next numbered thermal-set
publication, so one generation always names immutable contents.

`monitor_read_snapshot(out, after_generation)` copies a coherent complete
snapshot only when its generation exceeds the caller's value. Other statuses
leave output unchanged. A short SMP critical section protects publication and
copying; no blocking core call or register-mutex acquisition occurs there.
Copies remain historical snapshots when a newer generation appears. Failure,
stop and lifetime expiry revoke copied-output eligibility, including queued USB
success. Applications must use task context and keep output disjoint from
service/provider storage. These privileged APIs are not an unprivileged syscall
boundary.

`monitor_request_stop()` closes sampling and publication admission. An already
admitted bounded owner step can finish, but its returned sample is discarded.
Each owner then masks its interrupt and invokes its controller's bounded stop
operation under live ownership and the shared register mutex. GPADC disables
data interrupts, ADC enable and selected channels. Thermal disables alarm/shut
interrupts and all sensor enables; its data interrupts remain disabled. Both
operations verify disabled readback before acknowledgment. The service reports
STOPPED only after both acknowledgments before the absolute stop deadline.
This means configured-disabled converters. It does not prove an in-flight
analog conversion drained or authorize DMA reclamation.

Stop preserves clocks, reset state, rail borrow, buffers, tokens and tasks.
Repeated calls after stop perform no controller or provider I/O. There is no
restart or release. A stop requested during incomplete startup fails and retains
resources instead of continuing startup to manufacture a stop acknowledgment.
Uncertain writes/readbacks, owner faults, shared-provider failure, liveness
expiry or stop deadline failure permanently revoke both consumers. Owners then
retry retention metadata only. A stalled MMIO cannot be recovered by a software
deadline. Physical use requires authenticated controller exclusion, safe analog supplies,
passive divider inputs, register/security mappings, the reviewed supervised
RAM-only recovery plan
and confirmed whole-board power removal before resource reuse.

`apps/monitor/prepare.py` authenticates the complete 832-byte session and its
physical prerequisite records against the final image. Common and local full
session seals run before provider construction. `apps/monitor/capture.py` uses
the existing supervised queue, target lock, USB access and launcher. It captures
repeated durable snapshots for a requested interval, requests stop, and accepts
success only after both devices show repeated progress and STOPPED becomes
durable. Software-only fixture bundles cannot establish physical prerequisites.

Build the `sunstone-a733-monitor` target with `SUNSTONE_A733_MONITOR=ON` and
`SUNSTONE_BOARD_SUPERVISED=ON`. Preparation requires the final ELF and manifest,
an authenticated session template, an admission record and pinned compiler/linker
paths. Run `apps/monitor/prepare.py --help` for those required arguments. The
capture command requires the prepared bundle, FEL tool, evidence directory,
coordinator database/claim/owner and `--supervised-manual-reconnect`.
`--sample-seconds` selects a positive interval of at most 45 seconds; its default
is 5 seconds. The remaining supervised lifetime must also cover startup and the
bounded stop acknowledgment.

Thermal policy, throttling, other GPADC modes, LRADC, accessory-dependent inputs,
power transitions and broader concurrent services remain required work. This
fixed service does not replace those functions.

Software checks under
`~/.cache/sunstoneos/evidence/monitor-application-20261018/` pass thirteen
configured-stop cases, atomic parent coexistence and nominal producer checks,
complete admission/seal validation, twenty linked composition/service/USB cases
and four host groups. One native image links. The linked cases execute actual
clock-registry publication, parent register access, service copying and USB
serialization instructions with modeled controller returns, PMIC/rail/GIC
outcomes, task scheduling and transport. Separate sanitizer cases execute both
actual controller stop paths against register models. Earlier controller,
conversion, topology and provider evidence is reused. These results do not
qualify physical sampling, electrical admission, interrupt delivery or recovery.
