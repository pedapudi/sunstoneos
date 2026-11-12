# Memory, DRAM restoration and DMA

**Status: implemented.**

Native mappings, privileged reservation pools, cache maintenance, DMA0 services and shared IOMMU tables have host and emulator evidence. Full-capacity memory and device visibility are unqualified.

**Software evidence:** host, emulator.

**Board-qualified scope:** Full training and explicit-record restores on both boards report 12,288 MiB and pass two 4 KiB page comparisons. SID-selected restoration on 3-1 passes twice with 879 PHY writes and quick checks of 34.116 and 34.117 ms. CPU-L/DSU boot clocks restore. EL3 entry restores MBUS to 600 MHz on both boards while preserving the checked pattern. The runner's default DDR builds run boot0's post-DRAM interconnect step (nine NSI words, boot0 order), and the 12,224 MiB memory probe passes through them on 3-1. Sustained DRAM reads on 3-1 (development mode): one A76 core 14.8 GB/s, all eight cores 8.25 GB/s.

| Capability | Status | Key figure | Evidence and design |
| --- | --- | --- | --- |
| Full DRAM training, both boards | qualified on the board | 12,288 MiB, two 4 KiB page comparisons | [fast boot](../designs/dram-fast-boot.md), `dram-fast-boot-training-timing` |
| Explicit-record training restore | qualified on the board | 879 PHY writes | [training restore](../designs/dram-training-restore.md), [records](../evidence/dram-training-restore.jsonl) |
| SID-selected restore by default | qualified on the board | quick check 34.116 and 34.117 ms | `dram-training-restore-default-1031` |
| Card-boot restore variant and paired records | host-tested | 20 card cases, 32 selector cases | `dram-training-restore-card-offline`, `dram-training-restore-host-selection` |
| CPU-L and DSU boot-clock return | qualified on the board | ROM clock point (494 MHz) | [training restore](../designs/dram-training-restore.md#the-wrapper-returns-cpu-l-and-the-dsu-to-the-rom-clock-point) |
| MBUS at 600 MHz at EL3 entry | qualified on the board | 600 MHz, pattern preserved | [MBUS restore](../designs/mbus-restore.md), [records](../evidence/mbus-restore.jsonl) |
| Boot0 interconnect step in the DDR wrapper | qualified on the board | nine NSI words in boot0 order; 12,224 MiB probe passes | [bandwidth](../designs/dram-bandwidth.md), `1793603309777289754-ca40363d` |
| Sustained DRAM read bandwidth | qualified on the board | one A76 core 14.8 GB/s, eight cores 8.25 GB/s of 16.32 GB/s | `1793600409082394041-436818e2` |
| CPU memory latency, CPU-only load | qualified on the board | A76 chase 152 ns idle, 416 ns streaming; no deadline miss | `1793604009213366498-b525353f` |
| CPU memory latency, NPU and DMA load | open | not run | `dram-bandwidth-qos-knobs` |
| Memory-bus priority hook, latency window, DDR trip | host-tested | no image links the hook | [controller](../designs/dram-controller.md), [bus priority](../designs/dram-bus-priority.md) |
| Native mappings, reservation pools, cache maintenance | host-tested | emulator | [memory map](../memory-protection.md) |
| Full-capacity memory and device visibility | open | | [memory qualification](../memory-qualification.md) |

## Open work

1. Measure CPU memory latency with the NPU job stream and DMA masters loading memory and the CPU port priorities raised ([DRAM bandwidth open work](../designs/dram-bandwidth.md#open-work)); the probes that did this (`apps/dram-qos`, removed from the tree; see its history) need an NPU application that registers its graphs and a captured DMA session record. CPU-only runs show no effect. Separate the A55 core 0 latency tail from the OS tick.
2. Qualify full-capacity memory and device visibility, and physical range ownership with external-pool mapping.
3. Cover restore temperature, lower DRAM points, card deployment and decoding of the boot0 memory configuration ([training restore open work](../designs/dram-training-restore.md#open-work)). Read the controller's row-hit and bank-conflict counters if documented.
4. Establish physical static secondary-OFF ownership for the [native probe contract](../board-probe-contract.md).
5. Qualify physical DMA and USB return.

## DMA

**Software evidence:** host, emulator.

**Board-qualified scope:** None.

DMA0, DMA1 and IOMMU operations have no physical qualification. Host checks pass for descriptor bounds, DMA0 admission, retained-allocation exclusion and IOMMU table construction. FreeRTOS emulator checks pass for DMA service ownership and cyclic shutdown ordering. DMA0 supports bounded descriptor chains, 35-bit addressing, polled and interrupt completion, copied and cyclic receive services.

DMA0 needs physical channel, controller and data-access authority and transfer tests. DMA1 is blocked by an unresolved CPUS request and DRAM encoding. Native master exclusion, drain, installation and translation need physical evidence; no transfer above 4 GiB is qualified. DMA-engine load on the memory system needs a captured DMA session record. See [memory map](../memory-protection.md), [DMA](../dma.md), [IOMMU](../iommu.md) and [storage](storage.md) for the physical SD DMA reads.

## Evidence

Board results are records in [memory-dma.jsonl](../evidence/memory-dma.jsonl); add one with `tools/evidence.py add`. Related records: [dram-bandwidth.jsonl](../evidence/dram-bandwidth.jsonl), [dram-training-restore.jsonl](../evidence/dram-training-restore.jsonl), [mbus-restore.jsonl](../evidence/mbus-restore.jsonl), [dram-fast-boot.jsonl](../evidence/dram-fast-boot.jsonl), [dram-controller.jsonl](../evidence/dram-controller.jsonl) and [dram-bus-priority.jsonl](../evidence/dram-bus-priority.jsonl). Earlier prose evidence is in [the historical log](../history/status-memory-dma-20261101.md).

<!-- evidence-table:begin -->
28 records in `docs/evidence/memory-dma.jsonl`; run `tools/evidence.py sync` to regenerate this table.

| Entry or record | Date | Port | Image | Outcome | Verdict | Measurements |
| --- | --- | --- | --- | --- | --- | --- |
| `1793597424798692951-6741863a` | 2026-11-02 | 3-1 | `4e4f51b1ed5f` | returned | failed | run_total 19.252 s, read_one_a55 5.36 GB/s, read_one_a76 12.69 GB/s, read_two_a76 11.37 GB/s, read_six_a55 9.03 GB/s, read_eight 8.44 GB/s |
| `1793600409082394041-436818e2` | 2026-11-02 | 3-1 | `d7d1b5e761f6` | result-mismatch | failed | run_total 19.902 s, sum_one_a55 5.5 GB/s, sum_two_a55 10.05 GB/s, sum_four_a55 10.77 GB/s, sum_six_a55 9.06 GB/s, sum_one_a76 14.83 GB/s, sum_two_a76 14.02 GB/s, sum_four_a55_two_a76 9.36 GB/s, sum_eight 8.24 GB/s, sum_six_a55_stagger_4096 9.06 GB/s, sum_two_a76_stagger_4096 13.82 GB/s, sum_eight_stagger_4096 8.24 GB/s, sum_six_a55_stagger_65536 9.01 GB/s, sum_two_a76_stagger_65536 13.36 GB/s, sum_eight_stagger_65536 8.23 GB/s, sum_six_a55_stagger_262144 9.06 GB/s, sum_two_a76_stagger_262144 12.87 GB/s, sum_eight_stagger_262144 8.25 GB/s, sum_one_a55_mode1 5.28 GB/s, sum_one_a76_mode1 14.7 GB/s, sum_six_a55_mode1 9.05 GB/s, sum_two_a76_mode1 13.28 GB/s, sum_eight_mode1 8.22 GB/s, sum_one_a55_mode2 4.2 GB/s, sum_one_a76_mode2 12.54 GB/s, sum_six_a55_mode2 8.64 GB/s, sum_two_a76_mode2 13.44 GB/s, sum_eight_mode2 7.91 GB/s, sum_one_a55_mode3 2.19 GB/s, sum_one_a76_mode3 6.73 GB/s, sum_six_a55_mode3 5.76 GB/s, sum_two_a76_mode3 8.53 GB/s, sum_eight_mode3 5.74 GB/s |
| `1793600534434449836-8f1652a3` | 2026-11-02 | 3-1 | `1a8e6ff37cc0` | returned | failed | run_total 20.572 s, sum_one_a55 5.46 GB/s, sum_two_a55 10.02 GB/s, sum_four_a55 10.76 GB/s, sum_six_a55 9.07 GB/s, sum_one_a76 14.81 GB/s, sum_two_a76 14.15 GB/s, sum_four_a55_two_a76 9.34 GB/s, sum_eight 8.27 GB/s, sum_one_a55_swprefetch 4.19 GB/s, sum_two_a55_swprefetch 6.62 GB/s, sum_four_a55_swprefetch 9.53 GB/s, sum_six_a55_swprefetch 8.36 GB/s, sum_one_a76_swprefetch 13.8 GB/s, sum_two_a76_swprefetch 13.75 GB/s, sum_four_a55_two_a76_swprefetch 8.93 GB/s, sum_eight_swprefetch 7.94 GB/s |
| `1793600624723356256-fb710c2d` | 2026-11-02 | 3-1 | `5c6bbac1b12b` | returned | failed | run_total 21.112 s, sum_one_a55 5.49 GB/s, sum_two_a55 10.01 GB/s, sum_four_a55 10.82 GB/s, sum_six_a55 9.07 GB/s, sum_one_a76 14.86 GB/s, sum_two_a76 14.08 GB/s, sum_four_a55_two_a76 9.36 GB/s, sum_eight 8.25 GB/s, sum_one_a55_mode5 5.17 GB/s, sum_two_a55_mode5 4.8 GB/s, sum_four_a55_mode5 7.32 GB/s, sum_six_a55_mode5 7.22 GB/s, sum_one_a76_mode5 12.45 GB/s, sum_two_a76_mode5 11.75 GB/s, sum_four_a55_two_a76_mode5 8.38 GB/s, sum_eight_mode5 7.45 GB/s, sum_four_a55_mode6 6.88 GB/s, sum_six_a55_mode6 5.93 GB/s, sum_two_a76_mode6 11.3 GB/s, sum_eight_mode6 5.84 GB/s |
| `1793600664425885881-a871e54d` | 2026-11-02 | 3-1 | `e271e3b62bb8` | result-mismatch | passed | run_total 12.065 s, scan_pages 6.25869e+06, signature_pages 3.12934e+06, scan_time 0.555 s, signature_time 0.975 s, address_failed 0, mismatches 0 |
| `1793601413670925677-13bb2fa0` | 2026-11-02 | 3-1 | `aa92cd354db2` | returned | passed | run_total 18.779 s, word_202446c 199173, word_202466c 199173, word_2023a14 0, word_2023c14 0, word_2023e14 0, word_2024014 0, word_2024214 0, word_202200c 0, word_202220c 0 |
| `1793601453701366920-e6b42159` | 2026-11-02 | 3-1 | `cb5f5507a6f2` | result-mismatch | failed | run_total 19.957 s, sum_one_a55_before 5.47 GB/s, sum_two_a55_before 9.88 GB/s, sum_four_a55_before 10.82 GB/s, sum_six_a55_before 9.02 GB/s, sum_one_a76_before 14.84 GB/s, sum_two_a76_before 13.33 GB/s, sum_four_a55_two_a76_before 9.36 GB/s, sum_eight_before 8.24 GB/s, sum_one_a55_after 5.46 GB/s, sum_two_a55_after 9.9 GB/s, sum_four_a55_after 10.75 GB/s, sum_six_a55_after 9.06 GB/s, sum_one_a76_after 14.82 GB/s, sum_two_a76_after 13.99 GB/s, sum_four_a55_two_a76_after 9.4 GB/s, sum_eight_after 8.27 GB/s |
| `1793601532141189570-bb534f24` | 2026-11-02 | 3-1 | `3dcc955f1e96` | returned | failed | run_total 20.106 s, sum_one_a55_before 5.49 GB/s, sum_two_a55_before 9.98 GB/s, sum_four_a55_before 10.95 GB/s, sum_six_a55_before 9.07 GB/s, sum_one_a76_before 14.84 GB/s, sum_two_a76_before 14.16 GB/s, sum_four_a55_two_a76_before 9.39 GB/s, sum_eight_before 8.24 GB/s, sum_one_a55_after 5.5 GB/s, sum_two_a55_after 9.96 GB/s, sum_four_a55_after 10.85 GB/s, sum_six_a55_after 9.03 GB/s, sum_one_a76_after 14.82 GB/s, sum_two_a76_after 14.14 GB/s, sum_four_a55_two_a76_after 9.36 GB/s, sum_eight_after 8.26 GB/s |
| `1793601581524930927-ca5ca9c7` | 2026-11-02 | 3-1 | `ccab62e48743` | returned | failed | run_total 20.118 s, sum_one_a55_before 5.5 GB/s, sum_two_a55_before 9.95 GB/s, sum_four_a55_before 10.84 GB/s, sum_six_a55_before 9.07 GB/s, sum_one_a76_before 14.84 GB/s, sum_two_a76_before 14.11 GB/s, sum_four_a55_two_a76_before 9.37 GB/s, sum_eight_before 8.23 GB/s, sum_one_a55_after 5.51 GB/s, sum_one_a76_after 14.82 GB/s, sum_eight_after 8.25 GB/s |
| `1793601713672636837-caad23bd` | 2026-11-02 | 3-1 | `9b1160dc1361` | returned | passed | run_total 19.352 s, sum_one_a55 5.5 GB/s, sum_two_a55 9.89 GB/s, sum_four_a55 10.68 GB/s, sum_six_a55 8.97 GB/s, sum_one_a76 14.83 GB/s, sum_two_a76 14.18 GB/s, sum_four_a55_two_a76 9.35 GB/s, sum_eight 8.26 GB/s, a55_l2_refill_to_consumed_eight 1.006, a55_stall_fraction_eight 0.897, a55_l2_refill_to_consumed_one_a55 1.002, a55_stall_fraction_one_a55 0.398 |
| `1793601793681778248-4e7e5868` | 2026-11-02 | 3-1 | `20c13110294c` | returned | passed | run_total 19.83 s, port2_priority 0, port6_priority 0, port18_priority 0, port19_priority 0, port20_priority 0 |
| `1793603309777289754-ca40363d` | 2026-11-02 | 3-1 | `20c13110294c` | returned | passed | run_total 18.468 s, word_202446c 264709, word_202466c 264709, word_2023a14 63, word_2023c14 127, word_2023e14 15, word_2024014 241, word_2024214 241, word_202200c 1, word_202220c 1 |
| `1793603320437612290-e5eca66b` | 2026-11-02 | 3-1 | `e271e3b62bb8` | result-mismatch | passed | run_total 11.996 s, scan_ticks 1.33193e+07, signature_ticks 2.33955e+07, address_failed 0, mismatch_count 0 |
| `1793603354809199340-a4ed0f70` | 2026-11-02 | 3-1 | `1b2b6e8734ca` | returned | failed | run_total 15.295 s, region_bytes 1.07374e+09 B, read_control_40mib 1.87 GB/s, read_gib_blocks 1.87 GB/s, read_mib2_blocks 1.87 GB/s, cpu_clock 494 MHz, l1d_tlb_refill_gib_blocks 262314, l2d_tlb_refill_gib_blocks 3, l2d_tlb_refill_mib2_blocks 513, dtlb_walk_gib_blocks 2, dtlb_walk_mib2_blocks 512, el0_cases_faulted 7, el0_cases_succeeded 6 |
| `1793603526511445173-2ba7f3fb` | 2026-11-02 | 3-1 | `6a8c39e68d53` | result-mismatch | failed | run_total 19.267 s |
| `1793603526749137798-e9bb5100` | 2026-11-02 | 3-1 | `2a26fb331814` | result-mismatch | failed | run_total 18.815 s |
| `1793603527003752888-c6eca39d` | 2026-11-02 | 3-1 | `acd030a1b5a6` | watchdog-reset | failed | run_total 18.828 s |
| `1793603527212274330-dcf070c1` | 2026-11-02 | 3-1 | `a1c426ac14b4` | refused-before-launch | inconclusive | run_total 0.675 s |
| `1793604009006779744-6a685222` | 2026-11-02 | 3-1 | `2fd76e2a81a0` | returned | passed | run_total 25.627 s, a76_p50_ns_s0 152 ns, a76_p9999_ns_s0 639 ns, a76_max_ns_s0 643 ns, a55_p50_ns_s0 136 ns, a55_p9999_ns_s0 2569 ns, a55_max_ns_s0 9271 ns, ctrl_p50_ns_s0 2302 ns, ctrl_p9999_ns_s0 3069 ns, ctrl_max_ns_s0 3042 ns, ctrl_misses_s0 0, a76_p50_ns_s1 152 ns, a76_p9999_ns_s1 639 ns, a76_max_ns_s1 632 ns, a55_p50_ns_s1 136 ns, a55_p9999_ns_s1 2711 ns, a55_max_ns_s1 8843 ns, ctrl_p50_ns_s1 2302 ns, ctrl_p9999_ns_s1 3069 ns, ctrl_max_ns_s1 3011 ns, ctrl_misses_s1 0, a76_p50_ns_s2 152 ns, a76_p9999_ns_s2 639 ns, a76_max_ns_s2 628 ns, a55_p50_ns_s2 136 ns, a55_p9999_ns_s2 2569 ns, a55_max_ns_s2 9438 ns, ctrl_p50_ns_s2 2302 ns, ctrl_p9999_ns_s2 3069 ns, ctrl_max_ns_s2 3015 ns, ctrl_misses_s2 0 |
| `1793604009213366498-b525353f` | 2026-11-02 | 3-1 | `861b2416c428` | returned | passed | run_total 25.796 s, a76_p50_ns_s0 416 ns, a76_p9999_ns_s0 1662 ns, a76_max_ns_s0 2005 ns, a55_p50_ns_s0 392 ns, a55_p9999_ns_s0 16553 ns, a55_max_ns_s0 32424 ns, ctrl_p50_ns_s0 7417 ns, ctrl_p9999_ns_s0 10230 ns, ctrl_max_ns_s0 10571 ns, ctrl_misses_s0 0, a76_p50_ns_s1 416 ns, a76_p9999_ns_s1 1598 ns, a76_max_ns_s1 1965 ns, a55_p50_ns_s1 392 ns, a55_p9999_ns_s1 16553 ns, a55_max_ns_s1 29009 ns, ctrl_p50_ns_s1 7417 ns, ctrl_p9999_ns_s1 10230 ns, ctrl_max_ns_s1 10061 ns, ctrl_misses_s1 0, a76_p50_ns_s2 416 ns, a76_p9999_ns_s2 1598 ns, a76_max_ns_s2 1979 ns, a55_p50_ns_s2 392 ns, a55_p9999_ns_s2 16553 ns, a55_max_ns_s2 30056 ns, ctrl_p50_ns_s2 7417 ns, ctrl_p9999_ns_s2 10230 ns, ctrl_max_ns_s2 10083 ns, ctrl_misses_s2 0, stream_gb_per_s 8.78 GB/s |
| `1793604795633560445-80bc01fb` | 2026-11-02 | 3-1 | `e271e3b62bb8` | result-mismatch | passed | run_total 12 s, scan_ticks 1.33194e+07, signature_ticks 2.33961e+07, address_failed 0, mismatch_count 0 |
| `1793632829779717086-f7d0d1cf` | 2026-11-02 | 3-1 | `44252e038bb7` | returned | passed | run_total 51.578 s, load_time 30.659 s, load_rate 17.51 MB/s, read_window_40mib 12.79 GB/s, read_region_1gib 12.66 GB/s, round_trip_same_space 4.4 us, round_trip_ttbr0_change 4.36 us |
| `1793633314510847134-f446c660` | 2026-11-02 | 3-1 | `20c1435da4b2` | returned | passed | run_total 91.581 s, load_time 24.386 s, load_rate 22.02 MB/s, load_read_phase 23.253 s, load_copy_phase 0.154 s, load_digest_phase 0.979 s, read_window_40mib 12.79 GB/s, read_region_1gib 12.66 GB/s, round_trip_same_space 4.42 us, round_trip_ttbr0_change 4.37 us |
| `1793637177478128324-7b885910` | 2026-11-02 | 3-1 | `676526231d56` | returned | passed | run_total 44.26 s, region_base 0 x80000000, read_window_40mib 12.78 GB/s, read_region_6gib 12.66 GB/s, write_window_40mib 12.51 GB/s, write_region_6gib 12.16 GB/s, copy_window_40mib 5.84 GB/s, copy_region_3gib_to_3gib 5.84 GB/s, integrity_words 8.05306e+08, integrity_mismatches 0, integrity_write_time 2.414 s, integrity_read_time 2.682 s |
| `1793638861895715015-a7176fec` | 2026-11-02 | 3-1 | `1e45c08facb9` | result-mismatch | passed | run_total 63.521 s, fault_status 0 x06, fault_address 0 x7fffffff |
| `1793638933466934204-4dc4152d` | 2026-11-02 | 3-1 | `5f7f9f84fd74` | result-mismatch | passed | run_total 50.985 s, fault_status 0 x0d, fault_address 0 x200000000 |
| `1793638990638144080-9a9aad2d` | 2026-11-02 | 3-1 | `42a609c0698d` | result-mismatch | passed | run_total 18.73 s, fault_status 0 x0d, fault_address 0 x80000000 |
| `1794491111774670751-4e0e6e46` | 2026-11-12 | 3-3 | `a8e2fce081bd` | returned | passed | run_total 22.835 s |
<!-- evidence-table:end -->
