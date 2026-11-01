# Driver writes require independent completion evidence

The completion audit covers all 243 tracked files under `drivers/`, including
213 C files. Each inventory names the register-writing functions, their manual
or pinned vendor-source contract, and the implemented wait. The inventories
also trace native callbacks to their MMIO barriers and vendor inter-write
delays. Files that delegate completion or contain no register writes are
listed explicitly.

| Inventory | Files | C files | Driver directories |
| --- | ---: | ---: | --- |
| [I/O](driver-completion-io.md) | 63 | 61 | ADC, audio, Bluetooth, I2C, networking, pin control, PWM, RTC, SPI, UART, watchdog |
| [Media](driver-completion-media.md) | 45 | 35 | Camera, display, video, deinterlacing, G2D |
| [Accelerators](driver-completion-accelerators.md) | 76 | 70 | GPU, NPU, DMA, IOMMU, cryptographic engine |
| [Clocks, power, storage and interconnects](driver-completion-power.md) | 59 | 47 | Clock, DRAM, power, thermal, storage, PCIe, USB |

The [driver validation guide](driver-validation.md) defines the review and
fixture requirements. Source inspection and host models establish software
ordering and failure behavior. Physical timing, engine drain and repeated board
operation require separate qualification.

The CPU clock driver waits for UPDATE to clear before comparing N. Its native
adapters complete writes with DSB, and its transition retains boot0's 20 µs
interval between staging N and requesting UPDATE. The full CPU clock check
passes against the current driver. The
[stale-readback regression](../checks/register-latency.py) executes repeated
raises and lowers at three modeled latencies. Temporary mutations reproduce
premature verification and UPDATE capturing N before the vendor interval.
The [native CPU adapter check](../checks/os-cpu-clock-native.py) executes both
compiled write adapters with CPU-side posted stores. Reverting DSB leaves an
accepted store outstanding when the callback returns.

The shared [C](../checks/register_latency.h) and
[Python](../checks/register_latency.py) register effect preserves pending status
and stale fields until explicit virtual time reaches completion. Register reads
do not advance it. Separate effects represent independent acknowledgments,
such as UPDATE completion followed by PLL lock. The Python helper also queues
CPU-side posted writes until explicit transaction completion. This boundary is separate from peripheral application: early
acknowledgment under Device-nGnRE can precede endpoint arrival. Existing models
that separate requests from independent completion remain in use.

Three independent reviews cover hardware sequence, correctness and recovery for
the protected driver changes. The component inventories identify the focused
checks, mutation evidence and their limits. Raw commands, logs and measurements
remain under `~/.cache/sunstoneos/build/model-fidelity-1031/`.
