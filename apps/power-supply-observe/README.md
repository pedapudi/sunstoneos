# Read-only observations through the shared PMIC provider

Status: **host-checked; physical launch blocked**. The legacy returning
platform used by this build does not supply the required watchdog/breadcrumb
launch contract. The coordinator must integrate that contract through the
owned development runner before using this image on a board.

The finite client observes DCDC1, DCDC8, DCDC9, BLDO5, CLDO1 and CLDO3 in that
order. It fills the missing BLDO5 selector observation and preserves raw bytes
for disputed DCDC8/DCDC9 codes. Disabled or undecoded supplies are complete
observations. They grant no electrical or radio admission.
See [the reference reconciliation](../../docs/designs/pmic-reference-reconciliation.md)
and [the reviewed design](../../docs/designs/pmic-supply-observation.md).

## Build and check without device access

From this worktree, use a fresh output directory:

```sh
py="$HOME/.cache/sunstoneos/venv/bin/python3"
build="$HOME/.cache/sunstoneos/build/power-thermal-1031"
"$py" -B apps/power-supply-observe/build.py \
  --platform-build "$build/os" --evidence "$build/supply-image"
"$py" -B checks/pmic-supply-decode.py --output "$build/supply-decode"
"$py" -B checks/pmic-supply-native.py \
  --image "$build/supply-image/build/probe.elf" --output "$build/supply-native"
```

The platform build is configured through `cfg.sh` with `MIMUS_DIR=` and
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`. The builder compiles the current provider
and its dependencies. It records compiler inputs, tool hashes, linked objects,
ELF and binary in `input-identities.json`; no stale provider objects are reused.

## Frozen development artifact

| Artifact | SHA-256 |
| --- | --- |
| `supply-image/probe.bin` | `5161cfb0bb1d9f556f168ff1bda0de8a855b2fe65c90ee622bb59970a9cfac8e` |
| `supply-image/build/probe.elf` | `cdf278be7436c777b8af2173f97d03ddd7c01f44acc33b7dc45c5c2d6a7e99c4` |

Paths are relative to the build directory above. Kind: single-core RAM-only
diagnostic. Entry/load address: `0x41000000`. The 1.5-second client deadline
covers provider initialization, six observations and request cleanup. It
starts after task entry and counter initialization. The modeled successful
client takes 82,803 µs; whole-image startup and logging time remain unmeasured.

## Coordinator proposal and expected stages

This is the first proposed power-stream run, independently on each selected
board. It follows live identity, permissions, DDR and entry admission. It
needs exclusive S_TWI0/PL0/PL1 ownership, accessible PMIC registers, excluded
remote writers and a qualified watchdog/breadcrumb wrapper. It performs no
PMIC data write, storage write, radio enable or CPU frequency change.

| Stage | Expected result | Failure signature |
| --- | --- | --- |
| Platform/task entry | `BEGIN native supply observation` | No BEGIN; platform or entry failure |
| Provider startup, client step 1 | `READY native PMIC provider` | Stage/status report; zero completed supplies |
| Lease acquisition, step 2 | One stable lease for six supplies | `rail acquire` or `rail poll` failure |
| Six observations, steps 3–8 | Ordered BEGIN/END blocks, six raw bytes per snapshot | Snapshot failure; pending request and lease retained |
| Cleanup, step 9 | Six reaps, client release, idle bus and inherited pin state | Cleanup or inherited-state failure |
| Completion | `completed supplies=0x0000000000000006` and `COMPLETE native supply observations` | Nonzero exit or missing completion |

The current artifact has no RTC progress breadcrumbs and therefore has no
approved physical command. The runner owner must add host-armed WDT0, retained
stage/failure breadcrumbs, watchdog-verified normal return and copied logs to
the existing `tools/dev-run.py` path, then freeze the resulting image and exact
command. A plain kernel-runner invocation would bypass that prerequisite.

After a qualified normal return, decode the copied log offline:

```sh
"$py" -B apps/power-supply-observe/decode.py --log /absolute/run/kernel.log
```

Expected: six records and `electrical_admission: false`. Historical chip and
revision bytes are 3 and 0. DCDC8 raw `0xe4` must remain undecoded. BLDO5's
selector is unobserved, so no exact fresh value is predicted. Stop at the
first transport failure; no retry, reconnection loop or automatic reset follows.
Successful client cleanup retains the platform provider's clock/pin ownership.
Failure retains uncertain resources for the coordinator's recovery procedure.
