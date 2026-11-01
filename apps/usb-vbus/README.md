# Empty TYPEC24 source diagnostic

This diagnostic is implemented and software-tested. Physical operation remains
unqualified. The coordinator admits an empty TYPEC24 connector on port 3-1 and
owns the PL8 cutoff before launch. The image measures FUSB302 VBUSOK and CC
comparators; these observations establish neither regulated voltage nor vSafe0V.

Include `apps/usb-vbus/probe.cmake` through `CMAKE_PROJECT_sunstoneos_INCLUDE`.
Build `sunstone-a733-usb-vbus-0`, `sunstone-a733-usb-vbus-1` and
`sunstone-a733-usb-vbus-10`. Mode zero keeps the source disabled. Powered modes
reserve 2 ms before their nominal duration for shutdown scheduling. The report
measures the interval from the first PL8-high write attempt through confirmed low.
The accepted measured intervals are 0.980–1.000 seconds and
9.980–10.000 seconds, respectively.

Both tasks use the existing precise deadline runtime with CNTV/PPI27. Their
maximum service gap is 2 ms. I2C transactions use the shared byte engine with a
5 ms deadline and 50 µs retries. Initialization adopts the shared APBS1
parent, records inherited registers and resets S_TWI1 within 20 ms. The local
400 kHz divider follows the selected parent; S_TWI0 and shared clocks stay
unchanged. Software reset must self-clear and the physical bus must become idle. The vendor requests 400 kHz; the image chooses
the fastest representable divider at or below that rate. CC measurements settle
for at least 100 µs. STATUS1 thermal and VCONN overcurrent bits stop the
source; the switch current limit has no independent software observation. VBUSOK must rise within 275 ms and fall within 650 ms.

Normal completion requires observed PL8 low, VBUSOK low, an idle I2C engine,
masked controller interrupts and a retired timer and guard task. The image
then gates and resets S_TWI1, restores its pin configuration and retains PL8
as a confirmed-low GPIO output for the runner. Fatal entry
uses the shared probe hook to clear PL8 without reading EL1 memory. A stalled
interconnect can prevent this write; the runner must observe and recover PL8.

The FUSB302 sequence follows `drivers/usb/a733_typec.c` and its pinned sources
in `docs/typec-pd-sources.json`. Board routing comes from the existing board
description. This image initializes no USB controller, PHY or DMA engine.
It makes no PMIC writes and claims no measured supply voltage.

Run `check.py --image IMAGE.elf --evidence CACHE_DIRECTORY` with the configured
Python environment. It executes the actual diagnostic state machine, linked
cutoff task and linked fatal hook against modeled hardware, then checks payload
decoding. The coordinator supplies physical evidence separately.

The payload decoder is `decoder.py`. Fatal site `0xa0` means native preparation
failed; `0xa2` means I2C or discharge cleanup remained incomplete. The retained
RTC fault record identifies the first failure. A nonzero payload failure reports
a controlled mismatch after complete cleanup.

Build with the cached configuration helper and private CMake hook:

```sh
~/.cache/sunstoneos/bin/cfg.sh "$PWD" "$VBUS_BUILD" \
  -DMIMUS_DIR= -DCMAKE_C_COMPILER=/usr/bin/clang-21 \
  -DCMAKE_ASM_COMPILER=/usr/bin/clang-21 -DCMAKE_CXX_COMPILER=/usr/bin/clang++-21 \
  -DPython3_EXECUTABLE="$HOME/.cache/sunstoneos/venv/bin/python3" \
  -DCMAKE_PROJECT_sunstoneos_INCLUDE="$PWD/apps/usb-vbus/probe.cmake"
cmake --build "$VBUS_BUILD" -j2 --target sunstone-a733-usb-vbus-0 \
  sunstone-a733-usb-vbus-1 sunstone-a733-usb-vbus-10
~/.cache/sunstoneos/venv/bin/python3 apps/usb-vbus/check.py \
  --image "$VBUS_BUILD/sunstone-a733-usb-vbus-1.elf" --evidence "$VBUS_CHECK"
```

The coordinator uses the runner with mandatory PL8 admission, return and
recovery checks before submitting any image. Its offline check has this form:

```sh
~/.cache/sunstoneos/venv/bin/python3 tools/board.py check \
  "$VBUS_BUILD/sunstone-a733-usb-vbus-0.elf" --kind probe --port 3-1 \
  --approved-by coordinator --decoder apps/usb-vbus/decoder.py \
  --evidence "$VBUS_PREFLIGHT"
```
