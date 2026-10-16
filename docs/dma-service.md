# Privileged FreeRTOS DMA service

The DMA service serializes DMA0 and I2S capture through one preemptible EL1
worker. EL0 tasks submit copied requests through static FreeRTOS queues.
Registered jobs contain bounded, private copies of transfer metadata. Buffer
reservations persist through verified drain and remain held until explicit
result reaping. Task abandonment and retained faults do not reclaim resources.

One two-core FreeRTOS emulator session passes worker/client ownership,
concurrent channels, late replies, autonomous completion and deadlines,
retained results and faults, and I2S producer shutdown before DMA drain.
The fixture executes native CPU translation and cache operations while modeling
device registers and microphone samples. Physical DMA and audio remain
unqualified.

## Startup registrations define client authority

Initialize `dma_service` once at EL1 before starting the scheduler. Supply
DMA0 callbacks, the native adapter context, authorized channels, a drain
timeout in DMA counter ticks, registered jobs and protected memory ranges.
The service creates its static worker and queues. Bind each client slot to a
live task handle and a mask of permitted jobs before scheduler startup.
The worker's startup hook grants EL1t only while constructing that private task.
Application tasks have no selectable privilege flag.

A job fixes its DMA channel, transfer segments, complete mappings and descriptor
allocation. RAM jobs admit only RAM-to-RAM transfers. An I2S job additionally
supplies a trusted format and platform callback profile. Profiles for I2S0
must use one DMA channel. Clients select job indices; they cannot supply
addresses, callbacks, controller pointers or channel authority in requests.

Initialization copies job records, segment arrays, mapping arrays and I2S
profiles into private storage. Original metadata can then be discarded.
The pointed-to DMA allocations, native adapter and callback contexts remain
reserved and pinned for the service lifetime. An I2S profile's callback cookie
is a lifetime reference to trusted platform state.

The initial software bounds are eight client tasks, sixteen jobs, eight
segments and sixteen mappings per job. Up to sixteen DMA0 channels can be
authorized. The protected-range table contains at most thirty-two entries.
These bounds limit static storage and work per request; they are not hardware
capacity claims. There is no general heap allocation or runtime job-table edit.

## Protected storage and retained jobs constrain admission

Protected ranges describe both CPU and physical intervals. Initialization
requires a range covering the complete service object, including queues,
worker stack, copied metadata and controller state. Every registered DMA
allocation must avoid both address views of every protected range.
The platform must also supply ranges for adapter and callback state and exclude
page tables, firmware and other reserved allocations from the native policy.

The platform establishes the CPU-to-physical facts for protected ranges.
The service does not discover mappings or reserve RAM. The native adapter
continues to validate admitted buffer translations, attributes and access
permissions at submission. Aliases and mapping changes outside that policy
remain platform violations.

Before submission, the worker compares the candidate descriptor and buffer
allocations against every retained service reservation. Read-only source
sharing is allowed; descriptor overlap and overlap involving a writer are
refused in either CPU or device address space. This check includes completed
jobs whose controller transfer pointer has already been released.

The current shared address space does not isolate malicious application tasks.
Clients must obey buffer ownership and avoid modifying private service state.
Registered task handles must remain live and must not be deleted or reused.
A suspended or abandoned task retains its channel, pending reply and results.
There is no timeout-based ownership transfer or automatic reclamation.

## Requests separate channel sessions from buffer reservations

A request contains an operation, a registered job index and a timeout field.
The job selects its fixed channel. FreeRTOS supplies caller identity.

| Operation | Behavior | Timeout field |
| --- | --- | --- |
| `OPEN` | Claim a channel session; for I2S, acquire clocks/pins and configure capture | I2S resource duration in microseconds; zero for RAM |
| `SUBMIT` | Reserve the registered allocations and begin one transfer | Finite duration in DMA counter ticks |
| `STATUS` | Report phase, result, submitted ticket and ownership | Zero |
| `CANCEL` | Stop a running transfer through its owning controller path | Zero; the configured drain timeout applies |
| `REAP` | Return terminal result metadata and relinquish the buffer reservation | Zero |
| `CLOSE` | Release a quiescent session after its result has been reaped | I2S resource duration in microseconds; zero for RAM |

I2S resource durations become absolute monotonic microsecond deadlines in the
capture core. DMA capture and drain use the existing controller's counter
units. For the native adapter these are physical-counter ticks at
`CNTFRQ_EL0`; callers must convert durations using that frequency.

Sending copies the request into a queue. Queue success means enqueued;
the reply reports admission or refusal. Once a `SUBMIT` is queued, its client
must not alter the registered allocations until refusal or terminal completion.
Completion is observable through `STATUS` with the result phase and a retained
buffer reservation. The owner may then consume completed payload data while
the reservation is held. Failed or canceled transfers do not provide an accepted
payload result.

Queueing `REAP` authorizes the worker to relinquish that reservation. The client
must finish payload access before queueing it. Reaping returns result metadata,
not a new exclusive CPU allocation: another authorized job may then reuse the
same memory. Further CPU access needs the platform's allocation ownership rules.
A successful reap does not close the channel session.

Each client has one outstanding reply ticket. A collection timeout preserves
that ticket and leaves output arguments unchanged. The task must collect it
before sending another request. Client waits must be finite; `portMAX_DELAY`
is refused. Reply storage belongs to the service, so no timed-out caller stack
is borrowed by the worker.

## Each channel has one hardware owner path

The worker advances every active channel before receiving another request.
While any transaction progresses, its queue wait is at most one FreeRTOS tick.
Scheduling and callback latency can extend that interval. Completion and
controller deadlines advance without client requests, and queued status
traffic cannot prevent the next round of channel polls.

RAM jobs call the existing serialized DMA controller. I2S jobs call the
[capture core](i2s.md), which controls FIFO preparation, producer enable,
request shutdown, cancellation and drain. The service never independently
polls an I2S-owned DMA channel. This preserves the requirement that receive
DMA requests and reception stop before the DMA controller can release buffers.
Other FIFO peripherals require their own producer adapter before admission;
raw FIFO requests are refused.

I2S supports one capture per open. After reaping, close and reopen before
submitting another I2S capture. Opening and closing can continue asynchronously
through worker polls. An unfinished open has no cancellation path; its bounded
resource deadline can enter retained recovery with partial clock/pin tokens.
Local close preserves the platform-lifetime audio parent leases described in
the [I2S binding](i2s-platform.md).

A retained fault keeps the session and any admitted buffer reservation.
Status, close and reap report the fault without further callbacks for that
channel. Other independent channels can continue through the same serialized
worker. There is no reset, retry or forced release API. Recovery needs separate
proof that the peripheral and bus are quiescent before reclaiming resources.

## Native deployment requires prepared platform resources

The service assumes reserved DMA RAM, stable translations and the native
memory/FIFO admission policy. DMA clocks and reset must already be prepared,
CPU channel routing established and DMA interrupt delivery masked for this
polled implementation. DMA0 register access uses `0x04601000`.
All participants must preserve these resources for the service lifetime.

I2S jobs additionally require the shared clock leases and pin ownership of
the [platform binding](i2s-platform.md). Parent preparation remains an explicit
platform responsibility. The service neither programs global DMA clock/reset
state nor substitutes a mutex for clock-resource authorization.

Independent watchdog recovery is required before physical use. Software
request deadlines cannot interrupt a stalled MMIO transaction. Native memory
mapping, cache behavior, peripheral request wiring, bus drain and microphone
operation still require board qualification.

## Focused emulator evidence covers service integration

The single two-core session uses two registered EL0 clients and one unregistered
observer. Only the private preemptible EL1 worker invokes DMA, native memory
or I2S callbacks. The fixture destroys original startup tables after
initialization and modifies a queued request's source object. Submitted work
continues from the service's copies.

The session checks a delayed reply, client authorization, concurrent channels,
buffer conflicts before submission and after drain, autonomous completion,
cancellation and expiry, and completed-result retention after a task suspends.
An I2S capture checks producer-before-DMA-start and producer-before-DMA-stop
ordering. A retained drain fault checks buffer/session retention and absence
of later failed-channel accesses. Startup refuses a physical alias of service
storage and a raw FIFO job.

The fixture uses the native adapter for CPU translation, FIFO admission and
cache maintenance. Existing controller, memory-policy, I2S format, clock/pin
and full worker-register migration evidence is reused. No lower-layer suite,
physical board test or performance measurement was repeated.

The root build includes the [CMake fragment](../cmake/dma-service.cmake) after
the existing SMP setup. It builds `dma-service-model` with a private generated
port and the existing fixture-only execution-level observation hook.

Run the affected target when these inputs change:

```sh
cmake --build BUILD_DIRECTORY --target dma-service-model -j4
timeout 10s ~/.cache/sunstoneos/host/usr/bin/qemu-system-aarch64 \
  -machine virt,gic-version=3,secure=off,virtualization=off \
  -cpu cortex-a55 -smp 2 -m 128M -nographic -monitor none \
  -semihosting-config enable=on,target=native \
  -kernel BUILD_DIRECTORY/dma-service-model.elf
```

The emulator must exit zero and print `PASS DMA service:`. Raw evidence,
input identities and the private build overlay are under
`~/.cache/sunstoneos/evidence/dma-service-20261016/`. The implementation has not
accessed the physical board.
