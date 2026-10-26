# Finite NPU completion through a deferred interrupt

This optional single-core profile runs the existing trusted convolution twice,
copies and compares both outputs, reaps each job, and completes terminal CLOSE
before local gate/reset shutdown and finite ROM return. The default polled
profile and its accepted evidence remain available. The compiler, model and
reference data remain pinned external inputs.

The ISR owns only recording the system counter, masking interrupt 97 and
notifying the service. One service worker owns event acknowledgment,
deadlines, cancellation, full-idle checks and output cache acquisition. The
client waits for each bootstrap and job on the service's held `WAIT` reply,
and the image logs each job's time from publication to the interrupt and its
NPU active cycles ([design](../../docs/designs/npu-irq-completion.md)). The finite client requires exactly three successful
notified completions: bootstrap and two inference jobs. Its returned log includes
that count and the service's stopped receipt. The private host requires both
receipts as well as the existing copied numerical outputs and return reports.
A timeout, unknown event, failed mask/rearm or uncertain stop retains resources.

The source is level-high SPI 65 (GIC interrupt ID 97), routed only to CPU0.
The EL3 installer saves its group, modifier, configuration, priority and route in
protected monitor storage. It installs Secure priority `0xd8`, observed as
Nonsecure priority `0xb0`, at the selected FreeRTOS API threshold `11 << 4`.
The ISR uses the selected SRE port's combined EOI path. Group ownership stays
stable while the source operates; only this SPI is enabled and LPIs are excluded.
That scope excludes GIC-600 erratum 2384374's concurrent SET/CLEAR transition.
Nonsecure IGROUPR reads are RAZ/WI, so group validation belongs to EL3.

Before CLOSE, the service establishes a propagated mask with no active handler.
Controller CLOSE requires idle and clear events, disables event reporting with
readback, disables the MMU and verifies full idle again. Only then may the
architecture stop callback clear this source's GIC pending bit and verify it
inactive and clear. CLOSE does not release ownership until that callback succeeds.
The finite application parks providers and suspends workers before return. EL3
restores the saved source configuration before restoring SPI enables. Failure at
any boundary prevents normal retirement; reservations remain owned.

Build outside the repository:

```sh
python3 apps/npu-interrupt/build.py \
  --evidence ~/.cache/sunstoneos/evidence/npu-interrupt-service-20261019/finite
```

The private builder enables `A733_NPU_INTERRUPT` and reuses the finite monitor,
return tail, memory geometry and model producer. Its execution profile is
`npu-interrupt-finite-return-v1`. Host preparation uses
`apps/npu-interrupt/host.py` and the distinct entry schema
`a733-npu-interrupt-finite-entry-v1`. It retains the original finite host's
protected transport, output/receipt validation and DRAM restoration policy.
Additional hashed interrupt admission must bind the exact image, build identity,
CPU0 route, event owner, rearm exclusion, source-off stop and restoration order.
No qualified defaults or live admission are generated.

`~/.cache/sunstoneos/evidence/npu-irq-1025/irq-checks.sh SOURCE OUTPUT` builds
the image at 492 and 852 MHz and runs its checks. `host-check.py` runs the
finite host's fixtures (`apps/npu-finite/host-check.py --interrupt`) with the
interrupt admission record, the interrupt receipts and the interrupt refusal
cases.

The native image links. Focused device tests cover controller/service ownership,
retained failures, cancellation and deferred shutdown with actual native memory
providers. Linked client checks execute the new receipt gates and emit the logs
used by host refusal cases. Eighteen root-owned instruction cases cover route,
runtime masking, dispatch, shutdown and the changed monitor return boundary.
Those checks model system registers,
GIC effects and selected scheduler/service boundaries; the complete service is
exercised separately. Evidence resides under
`~/.cache/sunstoneos/evidence/npu-interrupt-service-20261019/`.

The two monitor composition cases execute the actual exception handler with
a matched receipt and a pending-source refusal. They verify route restoration
before SPI enables and retention before restoration when interrupt 97 is pending.
Unchanged cache and ROM-tail evidence is reused; these models establish no
physical return.

No board, USB or queue access occurred. Physical IRQ delivery, NPU execution,
cache visibility, numerical output, shutdown and ROM return remain unqualified.

`build.py --restart --evidence PATH` builds the separate software-only baseline
composition with a normal local stop/restart between the two jobs. It retains
domain/SRAM ownership and uses four IRQ completions, including two bootstraps.
The existing host admission rejects its distinct profile; it grants no board
authority. See the [restart contract](../../docs/npu-startup.md#normal-restart-within-one-powered-lifetime)
and run the linked client check with `checks/npu-interrupt-client.py --restart`.
