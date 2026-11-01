# The saved PMIC bytes do not select a voltage table

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

The shared decoder preserves the complete selector and enable bytes. For codes
88–127 it publishes `decoded=0` and `programmed_uv=0`. Enable state remains
independent of decoder validity. This refusal applies to both DCDC8 and DCDC9;
it adds no rail write or operating point.

The U-Boot getter applies the bit-mask `ALIGN` macro to a 100 mV step in its
higher branch. That macro requires power-of-two alignment, so its result
does not establish the intended higher voltage range. The comparison preserves
the getter's actual results instead of correcting that arithmetic silently.

## Package names do not establish register-table applicability

The reference board schematic labels the PMIC AXP318. The cached BSP identifies
chip ID 3 and revision 0 as AXP8191 version A. Those software names do not
establish the fitted package's transfer function.

The [manufacturer's selection table](https://www.allwinnertech.com/index.php?a=index&c=product&id=129)
lists AXP318W and AXP8191 separately. AXP318W has a WBBGA 8 × 8 mm, 147-contact
package and 29 LDOs. AXP8191 has a QFN 9 × 9 mm, 76-contact package and 19 LDOs.
This distinction prevents treating an AXP318W driver as proof of AXP8191
equivalence. It does not identify the fitted board component.

The vendor page is an undated selection table, retrieved on 2026-10-31.
The host snapshot is `build/power-thermal-1031/references/allwinner-axp-series.html`
under `~/.cache/sunstoneos/`, with SHA-256
`088e29562f133b2227eb53c731acc2c139b609f1673460c0017ef2d7b41124b0`.
Its section is the AXP318W/AXP8191 selection-list rows. Vendor copyright applies;
the page is a development reference and supplies no runtime code.
The [component source record](../../apps/power-supply-observe/sources.json)
pins the two GPL-2.0-or-later driver references by URL, revision, section and hash.
The host check executes an extracted U-Boot getter outside tracked source,
preserving its notices. No vendor driver code is linked into the diagnostic.

## Host evidence preserves both interpretations

[The source comparison](../../checks/pmic-supply-decode.py) passes 1,024 native
selector/enable cases and 512 original getter results. It also exercises six
complete snapshots through the actual PMIC, rail, clock and pin managers.
AddressSanitizer, UndefinedBehaviorSanitizer and AArch64 compilation pass.
Inputs, preserved source notices, executables and results are in
`build/power-thermal-1031/supply-decode/` under the local cache.

[The linked client check](../../checks/pmic-supply-native.py) passes complete
enabled and disabled observations, an address NACK on request four and an
active deadline fault. Failure retains the pending request, client lease and
held provider mutex. Normal completion performs six reaps before releasing the
client lease. The modeled complete path takes 82,803 µs and 36 byte reads.
There are no regulator data writes. Scheduling, queue calls, counter values
and I2C effects are modeled; startup, logging and physical timing remain
unqualified. Results are in `supply-native/` under the same build directory.

The feature review found an unbound numeric rail ID in the log parser. The
parser now binds each block name to its enum ID, and a mutated native log is
refused. The affected nominal/parser case passes in `supply-native-parser/`.
The unchanged disabled and fault cases retain their earlier evidence.

## Electrical admission remains blocked

The next useful observation is the missing BLDO5 selector through the shared
provider. [The finite diagnostic](../../apps/power-supply-observe/README.md)
also preserves current DCDC1, DCDC8, DCDC9, CLDO1 and CLDO3 bytes independently
for each board. No selector write follows from its results.

Choosing a disputed transfer function needs authoritative documentation for
the identified fitted PMIC. Radio admission also needs the fitted supply route
and measured pad/rail voltage. The existing schematic/DTS disagreement cannot
be resolved by PMIC readbacks. Preserve PM2 and all rail settings until that
evidence exists.
