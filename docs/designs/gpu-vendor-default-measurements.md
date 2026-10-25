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
