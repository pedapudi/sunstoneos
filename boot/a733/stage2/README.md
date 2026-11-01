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
memory and adds no device access. The native monitor occupies 32,368 bytes,
264 bytes more than the stream's base build. It leaves 400 bytes before the
fixed cold report. These are static sizes; boot latency is unmeasured.

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
