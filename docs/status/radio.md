# Wi-Fi and Bluetooth

**Status: implemented.**

Native SDC1 CMD52/CMD53 transport, immutable firmware staging, AIC8800D80 bootstrap, Wi-Fi scan, association and WPA2, IPv4 clients and listener, DHCP and DNS, and reconnect handling have software evidence. Wi-Fi and Bluetooth share one module owner.

Every OS image links the radio station, and it starts in a session that grants the radio device (`SS_DEVICE_RADIO`, bit 8), which the application must declare. The scanning station that returns to the host is a separate returning image (`SUNSTONE_A733_OS_RADIO_SCAN`). The Bluetooth headset is a library linked into an image whose application declares the headset device ([design](../designs/one-os-image.md)).


**Board-qualified scope:** On the Orange Pi Zero 3W's AIC8800D80 module, through the one OS radio image and its stage limit: the first-run guard (PG and PM read 1.8 V), the vendor-order power-on of BLDO5 and CLDO1 at 1.8 V with matched shutdown readbacks, SDIO enumeration (function 1 and 2 identities), the chip and subrevision words, four-bit 25 MHz widening and the firmware upload to its start confirmation, runtime start with the binding check, and a passive 2.4 GHz scan that lists the access points heard (none on 5 GHz) with a normal return. The crypto engine's TRNG delivers 32-byte blocks in the radio OS image, **unqualified and for bring-up only**: health tests at hardware defaults, no characterization, and `entropy_qualified()` still refuses, so no production key may come from it. Association with a WPA2-PSK access point (stage limit 5) is board-qualified: the four-way handshake, the pairwise and group key installs, the port open, the orderly shutdown and the return, using the unqualified CE entropy for bring-up only. DHCP, DNS and data traffic have not yet run on the board: the first address-stage run stopped at the pairwise key add (open work 1).

## Capabilities

| Capability | Status | Key figure | Evidence or design |
| --- | --- | --- | --- |
| Guard, power-on, SDIO enumeration, identity, firmware upload at four bits and 25 MHz, runtime start | board-qualified | firmware confirmed about 16.7 s into the session; 2,880 table pairs | [radio.md](../radio.md), [records](../evidence/radio.jsonl) |
| Passive Wi-Fi scan with shutdown and return | board-qualified | 22 access points, all on 2.4 GHz | [records](../evidence/radio.jsonl) |
| CE TRNG entropy for bring-up (unqualified) | board-tested, unqualified | 32 bytes in about 4 ms | [records](../evidence/radio.jsonl) |
| Wi-Fi association with WPA2-PSK (CCMP), keys installed, port opened, orderly shutdown and return | board-qualified | link secured 0.42 s after connect | [design](../designs/radio-service-stages.md), [records](../evidence/radio.jsonl) |
| IPv4, DHCP and DNS (address stage, stage limit 6), reconnect | host-tested; first board run stopped at association | none | [radio-wifi.md](../radio-wifi.md), [design](../designs/radio-service-stages.md), open work 1 to 3 |
| Bluetooth HFP audio gateway with voice-recognition activation | host-tested | `drivers/bluetooth/bt_hfp_ag.c` 11,572 B with call control off | [design](../designs/bluetooth-voice-recognition.md), `bluetooth-voice-recognition-budgets` |
| WPA3 SAE association, top-rate SDIO, headset interoperability | open | none | open work 4 to 6 |

## Open work

1. Find why a key add's confirmation sometimes never arrives. In two of four runs that reached the pairwise key add (entries 1794738427185556712-c450b701 and 1794762013628333724-64b494b3), the status read after the write found nothing pending, the owner waited for the card's SDIO interrupt, and no latch reached the host within 6 s. In the runs that passed, every confirmation was already pending at that first read, so a wake from the card interrupt after an empty status read during association has not been shown on the board. The wait is in `drivers/net/aic8800_sdio_transport.c` (the empty `RX_STATUS` branch, which acknowledges the controller latch through `sunxi_sdio_ack_irq` in `drivers/storage/sunxi_sdio.c` before it waits). Next step: diff the vendor's interrupt enable and acknowledge sequence (`aicwf_sdio.c`: `aicwf_sdio_hal_irqhandler`, the D80 function 0 register 0x07 write and the interrupt configuration write at bus start) against ours, and add a host case in which the confirmation becomes pending between the empty status read and the acknowledge.
2. Run the address stage (stage limit 6: DHCP lease and one DNS lookup, [design](../designs/radio-service-stages.md#the-address-stage-leases-an-ipv4-address-and-resolves-one-name)) on the board once item 1 is resolved; its first run stopped at the key add (item 1). Its record carries the IP task's wake count and the cores of the IP task and the radio owner, which decide the IP task's placement. Then add a TCP exchange through the SDK socket API to a host on the local network; propose the peer and an address-free record before building it.
3. Qualify reconnect after a link loss on the board.
4. Add capability reads, CMD5 and CMD11 voltage negotiation, drive and speed selection, PERI1 UHS clocks and CMD19 tuning for top-rate SDIO. The existing four-bit 25 MHz mode has [host-tested INTID194 completion](../designs/network-owner-events.md); physical interrupt timing remains unqualified.
5. Complete WPA3 SAE: the arithmetic exists, while external authentication, protected management frames and SAE association remain.
6. Qualify physical entropy and key-state observations, RF traffic, coexistence, sustained receive deadlines, broader modes, fault recovery and headset interoperability.

## Evidence

Design: [headset voice recognition](../designs/bluetooth-voice-recognition.md) (`docs/evidence/bluetooth-voice-recognition.jsonl`).

Board results are records in `docs/evidence/radio.jsonl`; add one with `tools/evidence.py add`. Earlier prose evidence is in [the historical log](../history/status-radio-20261101.md).

<!-- evidence-table:begin -->
11 records in `docs/evidence/radio.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793739986203659373-932efd85` | 2026-11-03 | 3-1 | `8733236de272` | returned | passed | run_total 20.022 s, guard_asked 1, guard_observed 1, guard_refused 0, gpio_pow_val 3.46013e+07 raw, sgpio_pow_val 14 raw, platform_stage 34 raw, bldo5_enable_byte 188 raw, bldo5_selector 13 raw, bldo5_programmed 1.8e+06 uV, dcdc1_programmed 3.3e+06 uV, cldo1_programmed 1.8e+06 uV, function_count 2, cccr_revision 3, sdio_revision 4, sdio_ocr_offered 2.70113e+09 raw, sdio_ocr_selected 3.14573e+06 raw, rca 14603 raw, function0_manufacturer 51361 id, function0_device 130 id, function1_manufacturer 51361 id, function1_device 130 id, function1_class 7 class, function2_manufacturer 51361 id, function2_device 386 id, function2_class 7 class, shutdown_readbacks_matched 1 |
| `1793750775070389039-2a278fd8` | 2026-11-04 | 3-1 | `d095536d632b` | returned | passed | run_total 20.144 s, bldo5_needs_power_on 1, bldo5_already_on 0, bldo5_enable_byte 188 raw, bldo5_selector 13 raw, bldo5_programmed 1.8e+06 uV, dcdc1_programmed 3.3e+06 uV, cldo1_programmed 1.8e+06 uV, gpio_pow_val 3.46013e+07 raw, sgpio_pow_val 14 raw, shutdown_readbacks_matched 1, chip_word 4.21157e+09 raw, subrevision_word 2081 raw, chip_revision 7 raw |
| `1793751829256885762-dd962cad` | 2026-11-04 | 3-1 | `ff081d22b824` | returned | passed | run_total 23.57 s, card_wide 1, firmware_state_start_confirmed 1, firmware_error 0, firmware_phase_start 6 phase, last_confirmation_ms_mod_65536 16749 ms, table_pairs_done 2880 pairs, image_bytes_sent_last 0 bytes, shutdown_readbacks_matched 1, chip_word 4.21157e+09 raw, subrevision_word 2081 raw |
| `1793752769560538673-7b89ebe1` | 2026-11-04 | 3-1 | `66f9a0bd3763` | returned | passed | run_total 24.974 s, aps_listed 22, aps_reported 31, aps_dropped 0, aps_2_4ghz 22, aps_5ghz 0, last_confirmation_ms_mod_65536 16370 ms, table_pairs_done 2880 pairs, shutdown_readbacks_matched 1, record_bytes 1200 bytes |
| `1793761734761083932-a96b3c78` | 2026-11-04 | 3-1 | `dcd4930f6aa0` | returned | passed | run_total 19.784 s, bytes_returned 32 bytes, task_us 4001 us, task_status 0 raw, task_error 0 raw, ce_reg_lpc 2 raw, step_limit_us 2e+06 us, ce_clk_reg 2.16426e+09 raw, ce_bgr_reg 196611 raw, mbus_word 4 raw |
| `1793925365211028755-3af212b9` | 2026-11-06 | 3-1 | `6cbe14831d82` | returned | passed | run_total 25.941 s, aps_listed 13, aps_reported 14, aps_dropped 0, aps_2_4ghz 13, aps_5ghz 0, last_confirmation_ms_mod_65536 17048 ms, table_pairs_done 2880 pairs, shutdown_readbacks_matched 1 |
| `1793949162186791205-7bddae46` | 2026-11-06 | 3-1 | `d2db61db27dd` | returned | passed | run_total 29.606 s, run_total 29.61 s, aps_listed 15, aps_reported 16, aps_dropped 0, aps_2_4ghz 15, aps_5ghz 0, last_confirmation_ms_mod_65536 17887 ms, table_pairs_done 2880 pairs, shutdown_readbacks_matched 1 |
| `1794738427185556712-c450b701` | 2026-11-15 | 3-1 | `cc769b60e80e` | returned | failed | run_total 30.734 s, connect_after_arming 4.131 s, fault_after_arming 5.586 s, return_after_arming 5.683 s, eapol_received 2, eapol_sent 2, rsn_state 9 raw, expected_cfm 37 id, cfm_bytes 0 bytes, command_bound 1000 ms |
| `1794745255109339882-f869332f` | 2026-11-15 | 3-1 | `3e837321a798` | returned | failed | run_total 29.555 s, connect_after_arming 4.124 s, fault_after_arming 4.545 s, return_after_arming 4.643 s, eapol_received 2, eapol_sent 2, rsn_state 9 raw, expected_cfm 37 id, cfm_bytes 2 bytes, cfm_status 0 raw, trace_rx_last_id 37 id, command_bound 6000 ms |
| `1794751437798567373-be2dcf1b` | 2026-11-15 | 3-1 | `204b58a7782b` | returned | passed | run_total 30.023 s, connect_after_arming 4.13 s, authorized_after_arming 4.554 s, return_after_arming 4.663 s, eapol_received 2, eapol_sent 2, rsn_state 9 raw, aps_listed 7, aps_2_4ghz 7, aps_5ghz 0, table_pairs_done 2880 pairs, slowest_confirmed_id 36 id, slowest_confirmation 0 ms, trace_last_command 5124 id, trace_last_response 5125 id, orderly_shutdown_matched 1 |
| `1794762013628333724-64b494b3` | 2026-11-15 | 3-1 | `12e2cfab18e7` | returned | failed | run_total 35.946 s, connect_after_arming 4.119 s, fault_after_arming 10.48 s, return_after_arming 10.583 s, eapol_received 2, eapol_sent 2, rsn_state 9 raw, aps_listed 9, table_pairs_done 2880 pairs, key_add_wait 5999 ms, status_reads_after_write 1, last_status 0 raw, rx_records_after_write 0, radio_interrupts_after_write 0, host_saw_sdio_latch 0, failure_shutdown_matched 1 |
<!-- evidence-table:end -->
