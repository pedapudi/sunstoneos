# Measurements of the default GPU power path

These counts compare the GPU power controller before the default path
(`work/gpu-core6-c-1024` at `750fa092`) with the default path of
[gpu-vendor-default.md](gpu-vendor-default.md). They come from host models
and an instruction check, not from a board, except where a row names the
board run.

| Metric | Before | After | Source |
| --- | --- | --- | --- |
| Controller register writes per ON | 11 | 16 | `checks/a733-gpu-power.c` write trace |
| Controller register writes per OFF | 4 | 8 | same trace |
| Controller callbacks per ON, one clock sample per µs | 82 | 182 | `checks/a733-gpu-power.c` |
| PLL_GPU0 output opened after the lock, floored 24 MHz clock | not applicable | 600 counter ticks (25 µs; at least 480 required) | same check |
| Lock-wait reads per PLL lock on the board | not applicable | 4 | session `board-session-20261025-4/port-3-1`, `s10-gvs-run` |
| CORE6 status 8 after the observation started, on the board | never | 3,151 µs | same run |
| PMIC transactions before the first ON of a platform | 0 | 3 | `drivers/gpu/a733_gpu_platform.c`; `checks/power-domains-service.c` |
| EL3 writes and reads per image start, cold | 0 and 0 | 31 and 3 | `checks/a733-monitor-are.py` |
| EL3 writes per image start, DMA left running | 0 | 33 | same check, case `are-37` |

Evidence: `~/.cache/sunstoneos/evidence/gpu-vendor-default-20261025/`, with
the baseline GPU checks in `baseline/gpu-checks/`.

## Default images whose identity changes

The central targets and the supervised and resident OS images were built at
the branch base (`750fa092`) and at `a071620a`, each from a clean archive at
the same path, and compared by SHA-256 and by their loadable bytes
(`~/.cache/sunstoneos/evidence/gpu-vendor-default-20261025/images/`). Of 56
images, 34 change:

- **Code changes, through an EL3 entry's security writes (17):**
  `a733-cxx-check`, `a733-kernel-check`, `a733-memtest`, `a733-mimus`,
  `a733-mimus-usb`, `a733-power-probe`, `a733-storage-probe` and
  `a733-usb-runtime` (the returning monitor); `a733-smp-cached-diagnostic`,
  `a733-smp-observe`, `a733-smp-supervised`, `sunstone-a733-board`,
  `-camera`, `-hdmi`, `-mimus-native`, `-smp-cached-devices` and
  `-storage-maintenance` (the SMP cached boot). The board, camera, HDMI,
  mimus-native and storage-maintenance images also link the changed PMIC
  provider, owner, consumer and resource manager.
- **Code changes, through the GPU and supply path (10):**
  `sunstone-a733-gpu-render` (SMP cached boot, GPU controller, platform and
  supply); the supervised and resident OS images (SMP cached boot, PMIC
  provider, owner, consumer and resource manager; their default
  configuration links no GPU controller); `npu-startup-model` (PMIC
  provider, owner and resource manager); and the six GPU service models
  (`gpu-compute-service-model`, `gpu-freelist-service-model`,
  `gpu-irq-model`, `gpu-memory-model`, `gpu-power-service-model`,
  `gpu-service-model`), which link the changed GPU stand-in of
  `checks/pvr-gpu-service.c` and, for the power model, the controller.
- **Debug information only (7):** `a733-cxx-model`, `a733-kernel-model`,
  `a733-mimus-model`, `a733-mimus-replay-model` and `a733-monitor-model`
  build the monitor with `MONITOR_QEMU`, which compiles the writes out;
  `i2s-file-model` and `i2s-runtime-model` include a changed header. Their
  loadable bytes are unchanged.

The 22 unchanged images are `a733-smp-diagnostic`, `kernel-check` and the
DMA, I2S stream and duplex, NPU service, radio and SMP models. The commit
after `a071620a` adds the writes to `platform/a733-smp/boot.c`, so
`a733-smp-diagnostic` also changes in code from that commit on; the SMP
models do not compile that file.

Images built by application scripts change when rebuilt from this tree,
because they compile one of the EL3 entries: the four GPU diagnostic
variants (at `22501205`, after the rebase onto bringup `49bc00da`: identity
`01849e62…`, power-only `b163b8fc…`, wrapper-read `a2d05627…`, probe-read
`ae2afba7…`; the first two frozen in `board-bundles-20261025-9/port-3-1/`),
and the returning USB, RTC, finite NPU
and board-bundle images. Frozen evidence sets, including the provider-readiness
checkpoint `3adfa4ad…`, keep their images.
