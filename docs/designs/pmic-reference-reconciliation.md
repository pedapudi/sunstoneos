# The saved PMIC bytes do not select a voltage table

Status: **host-tested.** The decoder refusal is implemented; no board run
has measured the output voltage of DCDC8 or DCDC9.

The intended reader maintains A733 drivers and board admission. A selector is
the voltage code stored in a PMIC register; its decoded value describes a
programmed setting, without measuring the output voltage.

The saved second-board DCDC8 selector is `0xe4`, whose seven-bit code is 100.
The pinned U-Boot getter returns 1,800,000 µV. The pinned Linux AXP8191 table
returns 3,100,000 µV. Both sources identify selector register `0x19` and enable
register `0x10`, bit 7. DCDC9 has the same transfer-table disagreement at
selector register `0x1a`, with enable register `0x11`, bit 0.

| Masked code | U-Boot interpretation | Linux interpretation | Runtime result |
| --- | --- | --- | --- |
| 0–70 | 500–1,200 mV, 10 mV steps | Same | Decoded |
| 71–87 | 1,220–1,540 mV, 20 mV steps | Same | Decoded |
| 88–101 | 1,560–1,820 mV, 20 mV steps | 1,900–3,200 mV, 100 mV steps | Raw, undecoded |
| 102–103 | Getter returns 1,936–2,036 mV | 3,300–3,400 mV, 100 mV steps | Raw, undecoded |
| 104–127 | Getter extrapolates beyond the Linux range | No table entry | Raw, undecoded |

The shared decoder (`a733_pmic_decode()` in `drivers/power/a733_pmic.c`)
preserves the complete selector and enable bytes. For codes 88–127 it
publishes `decoded=0` and `programmed_uv=0`. Enable state is independent of
decoder validity. The refusal applies to both DCDC8 and DCDC9 and adds no
rail write or operating point.

The U-Boot getter applies the bit-mask `ALIGN` macro to a 100 mV step in its
higher branch. That macro requires power-of-two alignment, so its result does
not establish the intended higher voltage range. The comparison records the
getter's actual results and does not correct that arithmetic.

## Package names do not establish register-table applicability

The reference board schematic labels the PMIC AXP318. The cached BSP
identifies chip ID 3 and revision 0 as AXP8191 version A. Those software names
do not establish the fitted package's transfer function.

The [manufacturer's selection table](https://www.allwinnertech.com/index.php?a=index&c=product&id=129)
lists AXP318W and AXP8191 separately: AXP318W has a WBBGA 8 × 8 mm package
with 29 LDOs, AXP8191 a QFN 9 × 9 mm package with 19 LDOs. An AXP318W driver
is therefore no proof of AXP8191 equivalence, and the table does not identify
the fitted board component. The vendor page is a development reference and
supplies no runtime code. The
[component source record](../../apps/power-supply-observe/sources.json) pins
the two GPL-2.0-or-later driver references by URL, revision, section and hash.
The host check executes an extracted U-Boot getter outside tracked source,
preserving its notices. No vendor driver code is linked into the diagnostic.

## Host evidence

[The source comparison](../../checks/pmic-supply-decode.py) checks the native
decoder against both references. [The linked client check](../../checks/pmic-supply-native.py)
covers enabled and disabled observations, an address NACK and a deadline
fault; a failure retains the pending request, client lease and held provider
mutex, and normal completion reaps six snapshots before releasing the lease.
The client makes no regulator data writes. Scheduling, queue calls, counter
values and I2C effects are modeled; startup, logging and physical timing are
unqualified. Figures, hashes and result locations are in
[pmic-reference-reconciliation.jsonl](../evidence/pmic-reference-reconciliation.jsonl).

## Open work

1. Observe the missing BLDO5 selector through the shared provider with the
   [finite diagnostic](../../apps/power-supply-observe/README.md), which
   preserves current DCDC1, DCDC8, DCDC9, CLDO1 and CLDO3 bytes independently
   for each board and writes no selector. The run is described in
   [pmic-supply-observation.md](pmic-supply-observation.md).
2. Choosing a disputed transfer function needs authoritative documentation for
   the identified fitted PMIC. Radio admission also needs the fitted supply
   route and measured pad and rail voltage. PMIC readbacks cannot resolve the
   schematic and DTS disagreement. Preserve PM2 and all rail settings until
   that evidence exists.
