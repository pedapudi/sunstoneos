# GPU graphics measurements

These measurements compare the render worker's per-frame CPU work at the
integration base (`audit/integration`, commit `041c09a`) with the current
branch (commit `736ba24`). They check the current sources against the
budgets in [gpu-graphics.md](gpu-graphics.md). All values come from host
models; no board was used.

## Method

- **Frame-path emulation.** `checks/gpu-texture-frame.py` from the current
  branch compiled the worker sources of each commit with `--source` and ran
  them under Unicorn. Each scene renders two frames, and the tables report
  the second, steady-state frame unless a row says otherwise. The base
  sources store to and maintain the DRAM depth extent, which the current
  harness asserts against. The base run therefore used a copy of the harness
  with only that assertion removed.
- **`DSB SY` count.** The emulation's `aarch64_device_cache` is a stub with
  no barrier. On the device each maintenance call issues two `DSB SY`, so
  the count is the barriers in the measured code plus two per maintenance
  call.
- **Code and data size.** Both commits were configured with the shared
  configuration script and built for `sunstone-a733-gpu-render` and
  `gpu-vm-memory-model`; `llvm-size` read the results.
- **Unchanged sources.** The branch changes only
  `drivers/gpu/pvr_render_service.c`, `drivers/gpu/pvr_render_texture.c`
  and their headers. The GPU runtime, VM plan, free list and display code
  are identical at both commits, and the VM model ELF is byte-identical.
  Metrics measured only by the display replay model or the VM check are
  therefore equal to the baseline and were not rerun.

Scenes: 1920 × 1080 profile 4 with 16 batches and read-back; 1280 × 720
profile 2 with read-back; and 1280 × 720 profile 2 presented in place.

## Frame-path results

Values are listed as 1080p read-back; 720p read-back; 720p presented.

| Metric | Base | Current | Target | Within target |
| --- | --- | --- | --- | --- |
| CPU bytes stored by the worker, excluding the 64 KiB read-back chunk | 77.0 MB; 24.7 MB; 24.7 MB | 16.7 MB; 7.4 MB; 7.4 MB | 17 KiB plus 128 marker bytes for scanout; read-back adds 8.3 MB poison at 1080p | No |
| Stores into the DRAM depth extent | 33.6 MB; 0; 0 | 0; 0; 0 | 0 | Yes |
| CPU cache maintenance bytes | 68.5 MB; 21.1 MB; 21.1 MB | 25.0 MB; 11.1 MB; 11.1 MB | Bytes written plus 256 marker bytes | No |
| Cache maintenance calls | 14,819; 4,302; 4,302 | 149; 69; 69 | Not set | — |
| `AT` translations, steady frame | 66,457; 24,853; 23,053 | 0; 0; 0 | 0 | Yes |
| `AT` translations, first frame of a lease | 66,459; not recorded; 23,055 | 25,564; 6,926; 6,926 | 0 | No |
| Output words checked at completion | 2,074,624; 922,624; 922,624 | 1,024; 1,024; 1,024 | 2 marker comparisons | No |
| Copies per input byte | 3 | 2 | 1 per upload | No |
| Copies per displayed byte | 3 | 1 | 0 | No |
| Copies per read-back byte | 3 | 1 | 1 | Yes |
| Queue round trips per read-back frame | 254; 114 | 254; 114 | 2 | No |
| Worker instructions, upload to COMPLETE | 619.4 M; 79.4 M; 79.4 M | 9.53 M; 3.30 M; 3.30 M | 100,000 | No |
| Worker instructions serving read-back | 5.60 M; 2.48 M; 0.069 M | 5.28 M; 2.34 M; 518 | Not set | — |
| Worker tick waits between CPU-only steps | 65; 20; 20 | 0; 0; 0 | 0 | Yes |
| Worker polls per frame | 1,050; 327; 327 | 22; 14; 14 | 2 wake-ups | No |
| Worker waits for the GPU, each repeated once per tick of rendering | 2 | 2 | 0 beyond interrupts | No |
| Service requests before COMPLETE | 131; 59; 59 | 131; 59; 59 | Not set | — |
| `DSB SY` per frame | 29,645; 8,611; 8,611 | 304; 144; 144 | Measured first; then 2 `DMB OSHST` per kick | No |
| `DMB` per frame | 0 | 0 | 2 per kick | No |
| Store-release instructions per frame | 600; 460; 348 | 254; 114; 2 | Not set | — |
| Payload bytes after the render payload | 70.7 MB; 27.9 MB; 27.9 MB | 52.4 MB; 14.2 MB; 14.2 MB | 29.2 MB at 1080p read-back; 20.0 MB at 720p scanout | 720p only |

Base worker stores break down at 1080p as 16.6 MB into the texture area, 8.3
MB into the snapshot copy, 8.4 MB into the payload area outside the named regions, which holds the draft, 8.3 MB of output poison,
1.8 MB of geometry and 33.6 MB of depth. The design's baseline of 60.2 MB and
17.4 MB excludes the snapshot copy and the draft; with the same exclusion the
harness gives 60.3 MB and 17.4 MB. Current stores are the input copy (8.4 MB;
3.7 MB) and the poison fill (8.3 MB; 3.7 MB). The presented scene still
writes the poison fill because scanout slots are not implemented.

The design gives the baseline `AT` count as about 38,000 plus 8,100 for
read-back. The harness counts 66,457 at 1080p and 24,853 at 720p at the base
commit. The design's baseline was not reproduced; the current value of zero
on steady frames meets the target in either case.

## Image size and data

| Item | Base | Current | Change |
| --- | --- | --- | --- |
| `sunstone-a733-gpu-render` text | 462,860 B | 462,640 B | −220 B |
| `sunstone-a733-gpu-render` bss | 823,153,616 B | 823,153,872 B | +256 B |
| `pvr_render_service.c` text | 10,904 B | 10,712 B | −192 B |
| `pvr_render_texture.c` text | 9,368 B | 9,320 B | −48 B |

The 256 bss bytes are `texture_batches` in `struct pvr_render_service`: 16
batch records of 16 bytes. The design requires the worker to copy each batch
record into its own memory and validate and encode from that copy, so the
producer cannot change a record after validation.

## Metrics unchanged by this branch

These metrics depend only on sources that the branch does not change. They
equal the design's baseline and stay outside target until the named owner's
change lands.

| Metric | Value at both commits | Target | Owner |
| --- | --- | --- | --- |
| KCCB kicks and return-slot waits | 2 and 2 | 2 and 0 | GPU runtime |
| Indices admitted per frame | 65,535 at every configuration | The parameter-buffer formula | GPU runtime and profile 5 |
| GPU leaf PTEs; parameter buffer | VM plan unchanged | 6,660 and 4,410; 16,412 PTEs, no growth | GPU runtime |
| Display wakes per second; register accesses per present | About 1,000; 14 replay, 1 status read per tick, 22-read scan | 180; 2 status reads per boundary | Display |
| IPIs per presented frame; interrupt to RCQ submit | Not applicable | 4; 1 IPI, no tick wait | Primary agent and display |
| Lock hold counts | Not instrumented: the emulation stubs the RTOS | Not set | — |

## Assessment

Every measured metric is equal to or better than the base, except the
render image's bss, which grows by 256 bytes for the batch copy that the
design requires. Every metric outside its target has a closing item in the
design's measured-values table or its list of changes needed from other
owners: input lending, scanout slots with freshness markers, the coverage
board item, the one-reply READ, GPU interrupt delivery, the payload check at
OPEN, the multi-range maintenance call and `DMB OSHST`.

## Reproduction

With the base and current trees exported to `src-base` and `src-head`:

```
PYTHONDONTWRITEBYTECODE=1 ~/.cache/sunstoneos/venv/bin/python \
  src-head/checks/gpu-texture-frame.py --source src-head --output frame-head \
  --clang clang --lld ~/.cache/sunstoneos/host/usr/lib/llvm-21/bin/ld.lld
```

The base run uses the same command with `--source src-base` and the depth
assertion removed from the harness copy.
