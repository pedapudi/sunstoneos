# Measurements: retained per-core fault records and the watchdog reboot path

Status: **host and instruction-emulation measurements**; no board was used.
The budgets come from [cpu-fault-reset.md](cpu-fault-reset.md).

## Conditions

- **Base:** merge base `f4a272f6cfaa15597f2987f807a6efc1da6fdb32`.
- **Changed:** `8d71d1148a8600d5094e9112e5e0a49213375f65` with the
  changes outside the package's own files applied as one patch (SHA-256
  `e2b03c41d66e386fe06f37ed8d2374479f436ade76f9bdcb6a4ad26bbdbf6704`). The
  patch adds the `x2` marker and capture call to `continue.S`, the rotation
  call to `continue.c`, the renewal refusal to `apps/os/services.c`, the
  supervisor's exit to `apps/board/main.c`, the report operation to
  `apps/mimus-board/usb0.c` and the new sources to `cmake/a733-os.cmake`.
- **Image:** the resident OS image, configured with
  `-DSUNSTONE_A733_OS=ON -DSUNSTONE_A733_OS_RESIDENT=ON`. The base ELF
  SHA-256 begins `477ec045933a8a91`; the changed ELF SHA-256 begins
  `389412ffdd8fbef0`.
- **Single-core images:** the crypto and PWM single-core images, which
  compile `platform/a733/platform.c` through
  `apps/storage-read-single-core/platform.patch`.
- **Tools:** Clang 21.1.8, Unicorn with modeled system registers and SMC
  entry, and the host model of `services.c` with a modeled counter and WDT0.
  None models caches, bus timing or board behavior.

Commands, run from the changed source tree:

```
checks/fault-capture.py --image CHANGED.elf --baseline BASE.elf --output OUT
checks/reset-latency.py --baseline-services BASE/apps/os/services.c \
    --capture OUT/result.json --output OUT2
checks/usb0-service-burst.py --tinyusb TINYUSB --output OUT3
```

All three checks pass on the changed tree.

## Results against the budgets

| Metric | Base | Changed | Budget | Within |
| --- | --- | --- | --- | --- |
| Modeled parks that end in a reboot | 1,000 of 8,000 (core 0 only) | 8,000 of 8,000 | Every park on every core | Yes |
| Park to watchdog expiry | Unbounded on cores 1..7 | At most 3.999 s | 4.02 s | Yes |
| Park entries that store the pending byte | 0 of 72 | 72 of 72 (9 kinds on 8 cores) | All | Yes |
| Instructions from MPIDR decode to pending store | No store | 6 | 6 | Yes |
| Capture leaf instructions | None | 85 | 250 | Yes |
| Capture: stack, MMIO or lock use | None | None; each core writes only its slot and byte | None | Yes |
| Fields per record | `ESR_EL1` as detail | 11 for an EL1h vector fault, 10 for EL0 | As designed | Yes |
| EL3 entry to record publication | 122 instructions | 203 for a deliberate exit, 224 for a vector fault | No budget | n/a |
| Supervisor pass | 40 instructions, no pending load | 52 instructions; one plain `ldr`, no barrier or atomic | One load, no lock or barrier | Yes |
| Rotation, once per boot | None | 1,868 to 1,875 instructions | No instruction budget | n/a |
| Rotation bytes read | None | In the check's slot mix: 1,248 without the copied resident result, 1,384 with it | 1,152 | No |
| Rotation bytes written | None | 1,160 to RAM, 1,024 cleared in `.result` | 1,160 and 1 KiB | Yes |
| USB0 report | None | 1,160 bytes in 1 tick, 3 packets | 1 tick | Yes |
| Single-core fault line | 3 registers | At most 136 bytes, 4 registers, core and task | 160 bytes | Yes |
| Single-core patch | Applies with fuzz 0 | Applies with fuzz 0, offset 25 lines | Fuzz 0 | Yes |
| OS image `.text` | 845,856 bytes | 846,880 bytes (+1,024) | +1 KiB | Yes |
| OS image `.rodata` / `.data` | 41,353 / 3,168 | Unchanged | n/a | Yes |
| OS image `.bss` | 1,463,136 bytes | 1,464,416 bytes (+1,280) | +1,168 | No |
| New reservations | n/a | None; `.result` stays 16 KiB | None | Yes |
| Single-core image `.text` | 492,224 (crypto), 490,432 (PWM) | +1,024 in each | No budget | n/a |

The rotation read count in the check's mix includes eight slot reads of
128 bytes each, a second read of every slot that passes its magic and CRC
check while copying it, and two stage-2 report header words. The 1,152-byte
budget assumed one read per slot plus the 128-byte resident result.

## Where the size growth comes from

Symbol sizes from `nm` account for the OS image's growth:

- `.bss` (+1,280 bytes): `a733_fault_previous` (1,160), `a733_fault_pending`
  (8), the USB0 frame buffer growing from 1,088 to 1,160 bytes (+72), the
  supervisor's stop-detail word (8) and 32 bytes of section alignment. The
  heap's device pool shrinks by the same 1,280 bytes, so the image's RAM
  footprint is unchanged.
- `.text` (+1,024 bytes): `a733_fault_boot` (388), `a733_fault_capture`
  (340), the renewal and supervisor changes in `board_resident_renew` (+48),
  `board_resident_stop_detail` (12), `supervise` (+16) and the services
  state function (+124), the USB0 `exchange` (+72), `a733_cached_prepare`
  (+4), and the vector-table markers, padded to the section alignment.
- Single-core images (+1,024 bytes of `.text`): `platform.c` grows from
  1,546 to 2,551 bytes because `platform_exception` now formats the record
  through `a733_fault_line` and `a733_fault_line_hex`, which print
  `SPSR_EL1`, the core number and the task name in addition to the three
  registers printed before.

## Assessment

Every reboot and capture budget is met. Two figures exceed their budgets,
and the design's "Implementation and measurements" section states the
reason for each:

- The rotation reads the stage-2 header words and each valid slot twice,
  exceeding the 1,152-byte read budget by up to 232 bytes. It runs once per
  boot, before any secondary core starts.
- `.bss` exceeds its 1,168-byte budget by 112 bytes: the larger USB0 frame,
  the stop-detail word and alignment.

The EL3 path to publication grows from 122 to 203 or 224 instructions, and
the supervisor pass grows from 40 to 52 instructions. The first runs only
when a core parks; the second adds one plain load and the expired-service
index, which the design specifies. The single-core images' 1 KiB of
`.text` growth carries no budget; the design attributes it to the added
fields of the fault line. No interrupt, wake-up, lock, barrier on the
supervisor path, cache maintenance or copy on a hot path is added.

Board evidence remains outstanding: WDT0 expiry and cold reboot, slot
retention across reset, and the reported registers of a real fault.
