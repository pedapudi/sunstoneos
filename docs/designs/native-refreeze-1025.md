# Design: running the returning diagnostics on the native image built from bringup

Status: **implemented and checked on host models; not run on a board.** The
board sequence is [the qualification runbook](../board-runbooks/refreeze-qualification.md).

The returning diagnostics patch a frozen native image and add an overlay.
Until now that image was `762237cd…`, frozen on 2026-10-18. It predates the
corrected EL1 entry: VPIDR_EL2, VMPIDR_EL2 and CNTHP_CTL_EL2, the three
active-priority copies, the Secure priority mask and SCTLR_EL1.nTWE, and the
per-core controller's wait for GICD_CTLR.RWP
([multicore correctness](multicore-correctness-1025.md)). The overlay copied
some of those steps; the controller's wait was absent from every returning
run.

## The new image

`a733-smp-supervised.elf`, built from a clean archive of `bringup` at
`d502f8ea`, has SHA-256 `a8228eaa…` and build ID `5a454f77…`
(`~/.cache/sunstoneos/evidence/refreeze-20261025/native/`, with its input
manifest and provenance). `tools/prepare-primary-publication-probe.py` pins
it. Its admission template (`refreeze-20261025/template/`) copies the
2026-10-18 first-task records with the image hash replaced; its launcher is
byte-identical to the pinned launcher.

## What the builders and host modules pin

The new image moves or changes these words; each is an exact pin, checked
against the image when a profile is built:

| Pin | Old | New |
| --- | --- | --- |
| First EL1 interrupt-interface write (`MSR ICC_CTLR_EL1, XZR`), the cached patch point in both overlay builders | `a733_cached_enter_el1+0x144` | `+0x194` |
| First-task patch setting SCTLR_EL1.nTWE (`+0xac`) | present | removed: the entry sets nTWE and nTWI |
| EL1 SCTLR expected by the overlay, the host decoders and the first-task model | `0x30d01805`, or `0x30d41805` with the patch | `NATIVE_EL1_SCTLR`, `0x30d51805`, in `boot/a733/secondary-local-probe.h` |
| Quiet profile's `BL ucPortGetCoreID` | displacement `0x3d61` | computed from the symbols |
| `worker` | `0x41001280` | `0x4100123c` |
| End of BSS | `0x41290d10` | `0x41290f00` (the owned writable interval still ends at `0x41291000`) |
| `a733_cached_el1_vectors` in the first-task saved context | `0x4100a000` | `0x4100a800` |
| Workload completion patch | `0x410012ec` | `worker+0x6c`, from the manifest |

The overlay keeps its own copies of the entry's EL3 steps; they now repeat
the native writes, which is harmless and costs 18 instructions per core.

## The instruction models follow the newer entry

Three behaviours of the native image are new to the models:

- the EL1 coherence helper reads the two sealed EL1 table pages and their
  stored sum, and cleans their lines, before it enables the MMU;
- the entry sets the Secure priority mask to 255 at EL3;
- the idle tasks wait with `DSB; WFI` at EL0 and are longer.

The base model now provides `sealed_el1_table_read` and
`production_table_clean` and the symbol sizes. The secondary, CPU0,
coherence, interrupt, pre-scheduler, first-task and workload models use them
instead of rules that assumed the old helper. In `stalled-core`, CPU0 can
wait in `identity_park` when the stalled core had already arrived; the
deadline interrupt then returns it with reason 1. The model records which
wait CPU0 reached, and the model and both host modules expect the matching
reason.

## Verification

From the source tree at `89dc4d4c` (`refreeze-20261025/source/`): all fifteen
profiles build, and the builds equal those of the working tree. The identity
model, which boots the new image from reset, and the 30 model cases pass.
The four CPU0 profile models and all fifteen host checks pass too. All
twelve bundles under `refreeze-20261025/bundles/` bind offline. No board
has run the new image; the qualification runbook is the physical test.
