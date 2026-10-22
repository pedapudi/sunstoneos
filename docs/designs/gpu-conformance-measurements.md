# Measurements: GPU output conformance against a host software renderer

These measurements compare the conformance harness with the commit it builds
on, against the budgets in [gpu-conformance.md](gpu-conformance.md). They come
from host builds and the host check. No board was used, so the board-evidence
rows stay unmeasured.

## Conditions

- Base: the merge base of this work and the integration branch, `c45a76e`.
  Head: `7ea7c6c`.
- Both trees were exported with `git archive` and configured with the shared
  aarch64 clang toolchain configuration. Both were built with
  `cmake --build -j 6` under `nice -n 10` in
  `~/.cache/sunstoneos/build/gpu-conformance-measure/`.
- The host check ran on the head tree as
  `checks/gpu-conformance.py --build <dir> --output <dir>`. It used the host
  Python virtual environment with `PYTHONDONTWRITEBYTECODE=1`. Its Mesa install
  is a copy of an earlier build of the pinned archive whose stamp matches the
  head pin. The reference renderer was compiled from the head source during the
  run.

## Firmware images

The change adds host programs and documents only. It touches no source file,
CMake file or header that a firmware image uses. The default build produces 13
ELF images. Base and head have the same total section size for each image. The
loadable bytes, extracted with `llvm-objcopy -O binary`, match byte for byte.
The two raw `.bin` outputs also match. Code size, data size, instruction
counts, barriers, lock holds, wake-ups and copies on the board therefore do
not change.

## Budget results

| Metric | Budget | Base | Head |
| --- | --- | --- | --- |
| Features with an output comparison | Every feature row in `docs/gpu-conformance.md` | 0 | 32 of 32 |
| Mutations detected | All ten | None | 10 of 10 |
| Board launches | 12 | No plan | 12 |
| Readback for all launches | At most 8 MiB; 7.1 MB planned | No plan | 7,143,540 bytes |
| Ticks per 512-byte packet | Plan parameter 2 | 1 to 2 | 2 |
| Worst-case frame stream | At most 15 s | None | 11.705 s, the 1920 × 384 launch |
| Worst-case session, including OPEN 15 s and CLOSE 1 s | At most 30 s | None | 28.104 s |
| Worst-case slot delivery | At most 0.5 s | None | 0.26 s |
| Worst-case request upload | At most 0.5 s | None | 0.258 s |
| 1920 × 1080 launch | Refused at the current USB rate | None | Refused: modeled frame stream 32.91 s |
| Board wall time per launch and slot | At most the modeled worst case | Not recorded | Not measured; needs board captures |
| Changes to board images | 0 | 0 | 0 |
| Reference processes per run | At most 5 | None | 5 |
| Comparator time, 1920 × 384 frame | At most 50 ms | None | 2.06 ms |
| Host check wall time after the Mesa build | At most 60 s | None | 13.16 s; 13.55 s for the process |
| Mesa rebuilds with an unchanged stamp | 0 | None | 0 |
| Edge-tie pixels in closed-form scenes | 0 | None | 0 |
| Edge-tie pixels in other scenes | At most 5% of covered pixels | None | 15,250 of 716,250 (2.1%) in the 1920 × 384 scene; 0 in the rest |

The host check passed. It also reported 11 refused plan violations. Its peak
resident memory was 119 MiB.

## Conclusion

Every host-measurable metric is within its budget. Each firmware image is
byte-identical to its base build, so no on-board performance metric changes.
Board wall time per launch and slot stays unmeasured until the twelve planned
launches run on the board.
