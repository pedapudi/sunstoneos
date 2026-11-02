# Wi-Fi and Bluetooth

**Status: implemented.**

Native SDC1 CMD52/CMD53 transport, immutable firmware staging, AIC8800D80 bootstrap, Wi-Fi scan, association and WPA2, IPv4 clients and listener, DHCP and DNS, and reconnect handling have software evidence. Wi-Fi and Bluetooth share one module owner.


**Board-qualified scope:** None.

## Open work

1. Establish the fitted supply topology: the schematic DCDC8 conflicts with the DTS BLDO5 and CLDO1.
2. Qualify radio identity, firmware loading, RF traffic, coexistence and headset interoperability.
3. Add capability reads, CMD5 and CMD11 voltage negotiation, drive and speed selection, PERI1 UHS clocks, CMD19 tuning and INTID194 completion for top-rate SDIO.
4. Complete WPA3 SAE: the arithmetic exists, while external authentication, protected management frames and SAE association remain.
5. Qualify physical entropy and key-state observations, shutdown, RF drain, sustained receive deadlines, broader modes and fault recovery.

## Evidence

Board results are records in `docs/evidence/radio.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-radio-20261101.md).

<!-- evidence-table:begin -->
No board result is recorded in `docs/evidence/radio.jsonl`.
<!-- evidence-table:end -->
