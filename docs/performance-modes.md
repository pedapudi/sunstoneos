# Performance modes

The optional performance service selects vendor operating points for the
CPU domains and, after local stop, the NPU. **High**, the default, selects
the highest point for the chip's speed bin. **Low** selects the fastest point
at the lowest vendor voltage. Thermal limits override either selection.
The GPU is reported but has no runtime owner in this profile.

Status: implemented and checked on the host; no board has run it yet. The
[board runbook](board-runbooks/performance-modes.md) lists the first run.

## The two modes

An operating point is a clock rate with the supply voltage it runs at. The
points below are those of speed bin vf0400, which both development boards
carry. The DSU (the shared L3 cache and snoop unit of the eight cores) follows
the two CPU clusters; you never select it.

| Block | High | Low |
| --- | --- | --- |
| Cortex-A55 cores (CPU-L) | 1794 MHz, 1000 mV | 1196 MHz, 800 mV |
| Cortex-A76 cores (CPU-B) | 2002 MHz, 1050 mV | 1404 MHz, 800 mV |
| DSU | 1274 MHz, 1000 mV | 910 MHz, 800 mV |
| GPU | 1008 MHz, 960 mV | 600 MHz, 800 mV |
| NPU | 852 MHz, 800 mV | 492 MHz, 800 mV |

A board of another speed bin has no table entry yet: the image reports the
rates it reads and refuses every switch. [The design](designs/performance-modes.md)
gives the source of every point.

## Images that have the service

The service is a build option of the supervised integrated OS image with the
Cortex-A76 cores admitted and CPU clock grants:

```sh
cmake ... -DSUNSTONE_A733_OS=ON -DA733_A76_ADMITTED=ON -DSUNSTONE_A733_OS_CPU_CLOCK=ON \
  -DSUNSTONE_A733_OS_PERF=ON [-DSUNSTONE_A733_OS_PERF_BOOT=low] [-DSUNSTONE_A733_OS_NPU=ON ...]
```

`SUNSTONE_A733_OS_PERF_BOOT` sets the mode the image starts in, `high` unless
you give `low`. The build's image manifest, `os-image/link-check.json`,
records it as `perf_boot_mode`, the service as `perf_modes`, and USB operation
8 as `perf_operation`. The host refuses older opcode-7 performance images.
Images built without the option do not have the service.

The session needs a CPU grant that names both clusters at the bin's high
points, bound to an approval record of schema
`a733-os-perf-modes-approval-v1` ([OS image](os-image.md#performance-mode-service-usb0-operation-8)).

## Using the command

`tools/sunstone-perf.py` talks to the running image over USB0. It needs the
board's USB port and the image manifest of the running build:

```sh
tools/sunstone-perf.py --port 3-3 --manifest BUILD/os-image/link-check.json status
tools/sunstone-perf.py --port 3-3 --manifest BUILD/os-image/link-check.json low
tools/sunstone-perf.py --port 3-3 --manifest BUILD/os-image/link-check.json high --block cpu-b
```

- `status` reads every block back from the hardware and prints it.
- `high` and `low` switch the blocks the image owns, or those that `--block`
  names (`cpu-l`, `cpu-b`, `cpu` for both clusters, `npu`), wait for the
  switch to finish, and print the result.
- `--json` prints the decoded report instead of the table.

The command exits with status 0 when the command completed without a refusal
or failure, and 1 otherwise. It takes the board lock of the port, so it
cannot run while a supervised launch holds the board; the launcher's
`--perf-sequence high,low,high` switches modes before each recognition
instead.

A `status` printout looks like this:

```
mode low (boot mode high), last command: read back, done
speed bin vf0400, CPU grant changes left 58
block  target            read back
cpu-l  1196 MHz  800 mV  1196 MHz  800 mV
cpu-b  1404 MHz  800 mV  1404 MHz  800 mV
dsu     910 MHz  800 mV   910 MHz  800 mV
gpu    not owned            0 MHz  960 mV    clock stopped
npu     492 MHz  800 mV   492 MHz  800 mV    clock stopped
```

The read-back values are the programmed PLL rates and PMIC selector voltages.
"clock stopped" marks a block whose clock gate is closed; its line shows the
rate its PLL would give.

## When a switch is refused or stops

The image refuses a switch before it changes anything when:

- the thermal level is not normal, or a sensor has not been read yet;
- another command is running (the command answers "busy" and retries for
  5 s);
- the named block is not owned by this image (the GPU, in every image so
  far);
- the NPU's session is still running 1.5 s after the request;
- the session's CPU grant has fewer than the needed changes plus two left
  for thermal restores. Each change of a CPU domain uses one of the grant's
  64 changes, so a session allows about 19 full switches.

A switch stops at the first block that fails. Blocks changed before it keep
their new point, and the rest keep their old one; the printout then shows
the mode as mixed, the failed block and the reason. A thermal interruption
invalidates the readback and the remaining plan, even if NORMAL returns
before the current change finishes. Run `status` after the driver settles
to establish a fresh readback before requesting another mode. A failure whose hardware state is unknown ends the session, as
every such failure in the OS image does.

## Limits

- **GPU.** No image can switch the GPU yet. The CPU clock owner runs only in
  supervised images; this profile has no GPU runtime owner. Demonstrated
  GPU startup and jobs do not establish runtime clock-change support. The
  GPU's clock and supply are still read back.
- **NPU.** The vendor changes the NPU clock only while the NPU is stopped,
  so the image changes it after its NPU session ends. The OS starts one NPU
  session per lifetime, so NPU work runs at the boot mode's rate; build with
  `SUNSTONE_A733_OS_PERF_BOOT=low` to run it at 492 MHz.
- **Thermal limits.** Under THROTTLE the CPU clusters drop to 1014 MHz
  when above that rate. At NORMAL the driver restores the points applied
  before throttling; an interrupted mode switch is not resumed.
