# Validate cold sessions before watchdog arming

The slot loader accepts a typed OS session only when its persisted board epoch
is zero and its generation matches the selected slot. It checks every extent
header, payload bound, duplicate type and padding byte before clearing a
retained result or arming WDT0. A CPU grant must have a zero epoch. An active
grant names the slot generation; an inactive grant keeps generation zero.

The loader then stamps the board epoch and hashes the complete session into
the resident contract. OS admission stamps its private CPU-grant copy with
that epoch. The persisted grant remains unchanged. Device policy, operating
points, manifest compatibility and physical authority remain OS admission
requirements.

The check runs once per boot over at most 4096 session bytes. It allocates no
memory and adds no device access. The native monitor occupies 32,168 bytes and
leaves 600 bytes before the fixed cold report; the card-gate entry below added
280 of them. These are static sizes; boot latency is unmeasured.

## Entry from the card first stage

The monitor accepts two entry states. In the first, WDT0 is disarmed and RTC
GP2 is zero. In the second, the card first stage has run its cold path
(`boot/a733/card-fel-gate.S`): GP2 holds its record with the high half `0x0c02`,
COLD set, WARM, UNMARKED and the refused-step bits (11:8) clear (ONCE and the
low byte are free), and WDT0 holds mode `0xb1` (16 s) with configuration 1 and
output 2. Any other state keeps the earlier refusal after loading.

In the second state the monitor restarts WDT0 first, then clears GP2 and
requires the clear to read back (a failed clear ends with status 7, detail 5),
and stores the record it read in report word `reserved1[22]` (byte offset 368).
While loading it restarts WDT0 after every card read, so the 16 s interval does
not expire during a normal load; the longest read is bounded by 0.5 s. Arming
then disarms WDT0 and arms mode `0x41` with the same configuration and output
writes and the same read-back and timing checks as in the first state; the
board has shown expiry only after an arming from mode 0
([card-boot-chain.md](../../../docs/designs/card-boot-chain.md)).

## Host verification

From the repository root:

```sh
cache="$HOME/.cache/sunstoneos"
build="$cache/build/sd-boot-1031"
"$cache/bin/cfg.sh" "$PWD" "$build/stage2-native" \
  -DMIMUS_DIR= -DSUNSTONE_A733_STAGE2=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build "$build/stage2-native" --target a733-stage2 -j4
"$cache/venv/bin/python3" -B checks/stage2-session.py \
  --build "$build/stage2-native" --output "$build/session-check"
```

Use a new output directory. The check uses the production typed-session
producer and slot-generation binder. ASan and UBSan cover exact-size buffers.
The linked AArch64 entry runs with modeled counter and watchdog registers.
Malformed sessions preserve the session, contract and retained result, with
no watchdog write. Valid sessions publish the expected epoch and digest.
The fixture starts after SD loading and hashing; it establishes no physical
SD transfer, cold boot or OS service-policy result.

Successful evidence from this stream is under the build directory:
`session-check-4/result.json`, `host-session/result.json`,
`slot-preservation/result.json` and `write-preservation/`.
The OS-only, hello-compute and external mimus build logs are alongside them.
Independent review found no blocking defect in the validator or linked fixture.

## Physical qualification remains pending

`a733-stage2.bin` is a cold boot0 monitor payload. Its predecessor and CPU
ownership checks exclude FEL entry. It must not be launched as a RAM-returning
image. Packaging and installation use the existing backup-first slot tools
after the coordinator establishes a real card allocation and full backup.
Storage-write approval, CPU and memory qualification, compatible resident
inputs and interruption recovery remain prerequisites.

On stale or malformed typed records, the stage-2 report at `0x47008200`
has status 7 and detail 2 before WDT0 arming. A session-size mismatch has
detail 1. Valid entry has status 6 and publishes the resident contract at
`0x42004000`. These DRAM reports are not RTC breadcrumbs and do not survive
power loss. Early SD failures precede resident watchdog arming; recovery from
those failures needs separate qualification.

The complete acceptance requirements remain in [the goal](../../../docs/goal.md).
