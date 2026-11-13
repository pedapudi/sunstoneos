# Verify native DMA interrupts and an idle owner

Status: implemented and host-tested. The native diagnostic objects compile;
the returning image and physical execution require coordinator integration.

This application uses the OS RAM-copy service. It opens that service, lets its
OPEN notification settle for one declared observation interval, then records
one second with no requests. The owner must retain its indefinite wait intent
and record no wake, timeout or interrupt in that window.

The application then submits two 4,096-byte copies with distinct byte patterns,
compares every output byte, reaps both results, closes and retires the service.
Each job must deliver its interrupt before the native worker observes completion.
The application checks the approved CPU operating points before the workload.
The normal return still requires the registered DMA quiesce proof.

The [private report](report.h) contains a 272-byte payload, including the two
idle counter snapshots, per-job interrupt and expiry counts, and the retired snapshot. [The decoder](host.py) checks
the copy count, the full idle duration, unchanged idle wake counters, job
interrupts without timer expiry and retirement. The controller retains its
existing policy for completion racing timeout; the diagnostic rejects that
boundary as interrupt qualification. Owner statistics use the public DMA class
getter; the application report remains specific to this diagnostic.
A timer used to observe a delayed channel drain also rejects the interrupt
qualification condition; normal ownership retention and drain checks still run.

The coordinator includes [the application registration](cmake/application.cmake)
from the `dma-events` branch of `cmake/a733-os.cmake`. Select
`SUNSTONE_A733_OS=ON`, `SUNSTONE_A733_OS_DMA=ON` and
`SUNSTONE_APPLICATION=dma-events`; build `sunstone-a733-os-dma-events`.
A session needs the typed 160-byte DMA record and actual
DMA0 handoff, access and recovery authority. The required facts are in
[the pool contract](../../docs/dma-pool-session.md); the application cannot
establish them. It performs no storage write.

[The host consumer check](../../checks/dma-events.py) models successful bus
movement, a corrupt byte, operation expiry and an idle wake. It also executes
the real completion adapter with stale reply hints and raced device progress.
The [worker check](../../checks/event-dma-worker.py) rejects completion polling
before an interrupt, including polling induced by STATUS requests. These models
establish software control flow and do not establish physical DMA behavior.
