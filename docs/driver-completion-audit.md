# Driver writes require independent completion evidence

The completion audit covers all 240 tracked files under `drivers/`, including
214 C files. Each inventory names the register-writing functions, their manual
or pinned vendor-source contract, and the implemented wait. Files that delegate
completion or contain no register writes are listed explicitly.

| Inventory | Files | C files | Driver directories |
| --- | ---: | ---: | --- |
| [I/O](driver-completion-io.md) | 63 | 61 | ADC, audio, Bluetooth, I2C, networking, pin control, PWM, RTC, SPI, UART, watchdog |
| [Media](driver-completion-media.md) | 42 | 35 | Camera, display, video, deinterlacing, G2D |
| [Accelerators](driver-completion-accelerators.md) | 76 | 71 | GPU, NPU, DMA, IOMMU, cryptographic engine |
| [Clocks, power, storage and interconnects](driver-completion-power.md) | 59 | 47 | Clock, DRAM, power, thermal, storage, PCIe, USB |

The [driver validation guide](driver-validation.md) defines the review and
fixture requirements. Source inspection and host models establish software
ordering and failure behavior. Physical timing, engine drain and repeated board
operation require separate qualification.

The CPU clock driver remains outside this change's ownership. Its multiplier
comparison precedes UPDATE completion. The
[stale-readback regression](../checks/register-latency.py) runs temporary build
copies with premature and waited verification orders through the real CPU,
PMIC, rail and thermal code. The premature order faults with the old multiplier
and UPDATE still set. The waited order completes repeated raises and lowers at
three modeled latencies. The source driver is unchanged.
The existing full CPU transition check also exposes the defect: its stuck-UPDATE
case faults before the update deadline because the multiplier still reads old.

The shared [C](../checks/register_latency.h) and
[Python](../checks/register_latency.py) register effect preserves pending status
and stale fields until explicit virtual time reaches completion. Register reads
do not advance it. Separate effects represent independent acknowledgments,
such as UPDATE completion followed by PLL lock. Existing models that already
separate requests from independent completion remain in use.

Three independent reviews cover hardware sequence, correctness and recovery for
the protected driver changes. The component inventories identify the focused
checks, mutation evidence and their limits. Raw commands, logs and measurements
remain under `~/.cache/sunstoneos/build/model-fidelity-1031/`.
