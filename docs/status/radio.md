# Wi-Fi and Bluetooth

**Status: implemented.**

Native SDC1 CMD52/CMD53 transport, immutable firmware staging, AIC8800D80 bootstrap, Wi-Fi scan, association and WPA2, IPv4 clients and listener, DHCP and DNS, and reconnect handling have software evidence. Wi-Fi and Bluetooth share one module owner.

Every OS image links the radio station, and it starts in a session that grants the radio device (`SS_DEVICE_RADIO`, bit 8), which the application must declare. The scanning station that returns to the host is a separate returning image (`SUNSTONE_A733_OS_RADIO_SCAN`). The Bluetooth headset is a library linked into an image whose application declares the headset device ([design](../designs/one-os-image.md)).


**Board-qualified scope:** On the Orange Pi Zero 3W's AIC8800D80 module, through the one OS radio image and its stage limit: the first-run guard (PG and PM read 1.8 V), the vendor-order power-on of BLDO5 and CLDO1 at 1.8 V with matched shutdown readbacks, SDIO enumeration (function 1 and 2 identities), the chip and subrevision words, four-bit 25 MHz widening and the firmware upload to its start confirmation, runtime start with the binding check, and a passive 2.4 GHz scan that lists the access points heard (none on 5 GHz) with a normal return. The crypto engine's TRNG delivers 32-byte blocks in the radio OS image, **unqualified and for bring-up only**: health tests at hardware defaults, no characterization, and `entropy_qualified()` still refuses, so no production key may come from it. Association and the four-way handshake (stage limit 5) are built and not yet run on the board.

## Capabilities

| Capability | Status | Key figure | Evidence or design |
| --- | --- | --- | --- |
| Guard, power-on, SDIO enumeration, identity, firmware upload at four bits and 25 MHz, runtime start | board-qualified | firmware confirmed about 16.7 s into the session; 2,880 table pairs | [radio.md](../radio.md), [records](../evidence/radio.jsonl) |
| Passive Wi-Fi scan with shutdown and return | board-qualified | 22 access points, all on 2.4 GHz | [records](../evidence/radio.jsonl) |
| CE TRNG entropy for bring-up (unqualified) | board-tested, unqualified | 32 bytes in about 4 ms | [records](../evidence/radio.jsonl) |
| Wi-Fi association, WPA2, IPv4, DHCP and DNS, reconnect | host-tested | none | [radio-wifi.md](../radio-wifi.md) |
| Bluetooth HFP audio gateway with voice-recognition activation | host-tested | `bt_hfp_ag.c` 11,572 B with call control off | [design](../designs/bluetooth-voice-recognition.md), `bluetooth-voice-recognition-budgets` |
| WPA3 SAE association, top-rate SDIO, headset interoperability | open | none | open work 2 to 4 |

## Open work

1. Run the association stage on the board (`--radio-stage-limit 5`, with the unqualified CE entropy for bring-up only), then DHCP and a TCP exchange ([design](../designs/radio-service-stages.md)).
2. Add capability reads, CMD5 and CMD11 voltage negotiation, drive and speed selection, PERI1 UHS clocks, CMD19 tuning and INTID194 completion for top-rate SDIO (the board runs at four bits and 25 MHz).
3. Complete WPA3 SAE: the arithmetic exists, while external authentication, protected management frames and SAE association remain.
4. Qualify physical entropy and key-state observations, RF traffic, coexistence, sustained receive deadlines, broader modes, fault recovery and headset interoperability.

## Evidence

Design: [headset voice recognition](../designs/bluetooth-voice-recognition.md) (`docs/evidence/bluetooth-voice-recognition.jsonl`).

Board results are records in `docs/evidence/radio.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-radio-20261101.md).

<!-- evidence-table:begin -->
6 records in `docs/evidence/radio.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793739986203659373-932efd85` | 2026-11-03 | 3-1 | `8733236de272` | returned | passed | run_total 20.022 s, guard_asked 1, guard_observed 1, guard_refused 0, gpio_pow_val 3.46013e+07 raw, sgpio_pow_val 14 raw, platform_stage 34 raw, bldo5_enable_byte 188 raw, bldo5_selector 13 raw, bldo5_programmed 1.8e+06 uV, dcdc1_programmed 3.3e+06 uV, cldo1_programmed 1.8e+06 uV, function_count 2, cccr_revision 3, sdio_revision 4, sdio_ocr_offered 2.70113e+09 raw, sdio_ocr_selected 3.14573e+06 raw, rca 14603 raw, function0_manufacturer 51361 id, function0_device 130 id, function1_manufacturer 51361 id, function1_device 130 id, function1_class 7 class, function2_manufacturer 51361 id, function2_device 386 id, function2_class 7 class, shutdown_readbacks_matched 1 |
| `1793750775070389039-2a278fd8` | 2026-11-04 | 3-1 | `d095536d632b` | returned | passed | run_total 20.144 s, bldo5_needs_power_on 1, bldo5_already_on 0, bldo5_enable_byte 188 raw, bldo5_selector 13 raw, bldo5_programmed 1.8e+06 uV, dcdc1_programmed 3.3e+06 uV, cldo1_programmed 1.8e+06 uV, gpio_pow_val 3.46013e+07 raw, sgpio_pow_val 14 raw, shutdown_readbacks_matched 1, chip_word 4.21157e+09 raw, subrevision_word 2081 raw, chip_revision 7 raw |
| `1793751829256885762-dd962cad` | 2026-11-04 | 3-1 | `ff081d22b824` | returned | passed | run_total 23.57 s, card_wide 1, firmware_state_start_confirmed 1, firmware_error 0, firmware_phase_start 6 phase, last_confirmation_ms_mod_65536 16749 ms, table_pairs_done 2880 pairs, image_bytes_sent_last 0 bytes, shutdown_readbacks_matched 1, chip_word 4.21157e+09 raw, subrevision_word 2081 raw |
| `1793752769560538673-7b89ebe1` | 2026-11-04 | 3-1 | `66f9a0bd3763` | returned | passed | run_total 24.974 s, aps_listed 22, aps_reported 31, aps_dropped 0, aps_2_4ghz 22, aps_5ghz 0, last_confirmation_ms_mod_65536 16370 ms, table_pairs_done 2880 pairs, shutdown_readbacks_matched 1, record_bytes 1200 bytes |
| `1793761734761083932-a96b3c78` | 2026-11-04 | 3-1 | `dcd4930f6aa0` | returned | passed | run_total 19.784 s, bytes_returned 32 bytes, task_us 4001 us, task_status 0 raw, task_error 0 raw, ce_reg_lpc 2 raw, step_limit_us 2e+06 us, ce_clk_reg 2.16426e+09 raw, ce_bgr_reg 196611 raw, mbus_word 4 raw |
| `1793925365211028755-3af212b9` | 2026-11-06 | 3-1 | `6cbe14831d82` | returned | passed | run_total 25.941 s, aps_listed 13, aps_reported 14, aps_dropped 0, aps_2_4ghz 13, aps_5ghz 0, last_confirmation_ms_mod_65536 17048 ms, table_pairs_done 2880 pairs, shutdown_readbacks_matched 1 |
<!-- evidence-table:end -->
