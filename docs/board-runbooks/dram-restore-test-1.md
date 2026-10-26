# First DRAM training-restore test (FEL path)

Status: **built and host-checked; not run on a board.** The record builder,
SRAM restore helper and runner are implemented. The first physical test
still requires a fresh power-cycled lifetime per board and acceptance of the memory qualification image's three residuals.
Independent correctness review approved the offline implementation on
2026-10-25. No launch approval or recovery qualification follows from the
host checks or that review.

This boot-time experiment replaces boot0's 4.4 s P-state 0 training with the
initialization-only firmware run (40 ms on both boards) and a write of the
board's recorded training results, then shows on the board that DRAM works
at 2040 MHz. It writes no storage: the record comes from the host.

## The restore set comes from the existing lifetimes

Each board has two four-P-state, two P-state-0-only and one
initialization-only lifetime ([capture runbook](dram-training-capture.md)).
The set is fixed from those, in `~/.cache/sunstoneos/evidence/dram-training-analysis-20261025/restore-set-v3.json`,
by the single command recorded beside it in `restore-set-v3.command`
(`tools/analyse-ddr-captures.py` over all twenty captures):

- **Registers: 1,036, all 16-bit PHY registers or parameter words.** For
  each board, the registers whose P-state-0-only values all differ from its
  initialization-only values (993 on 3-3, 1,019 on 3-1), united. Status is
  decided over all twenty captures of both boards, which removes the
  controller word `0x0a51022c`; it changed within a port 3-1 lifetime.
- **Left out, with the reason:**
  - the `0xa0000` block (below);
  - the APB-only and DRTUB blocks, which are firmware controls;
  - `dram_tpr10` itself (parameter word 27), which the restore edits;
  - 27 message-block fields that hold one value in every P-state-0-only
    capture, such as the training-step value at `0x0a9b0010`: they are
    firmware inputs that the step value sets, not results;
  - the controller's mode-register-read result words `0x0a02002c` and
    `0x0a020040` and the MR14 copies `0x0a110094` and `0x0a510094`, which a
    restore cannot meaningfully write; MR14 travels in parameter word 50.
- **Two groups.** 410 registers vary between trainings: they are training
  results. 626 hold one value in every P-state-0-only capture of both
  boards and differ after the initialization-only run; they are the
  P-state-0-only path's training state, which the restore writes over the
  initialization-only path's. The earlier `restore-set-final.json`
  (1,069 registers) is superseded.
- **The `0xa0000` block stays out.** After a four-P-state training, 8–9% of
  its words change between lifetimes (941 on 3-3, 1,039 on 3-1), much as
  trained registers do. After a P-state-0-only training nearly every word
  of all four copies changes (about 3,500 per copy). So the four-P-state
  path writes the block and the P-state-0-only path leaves it at its
  power-on contents; the post-training call `0x581b4`, which runs only with
  `dram_tpr13` bit 2 set (`0x57a08`), is the likely writer (inferred). The
  restore runs the P-state-0-only path, so it leaves the block as that
  path does, and P-state-0-only training already passes boot0's simple
  test and the two page checks with it.
- **Values:** one capture of the board's later P-state-0-only lifetime
  (`-27` for 3-3, `-35` for 3-1). Values from one training are restored
  together, because a byte-lane register can move by one coarse step of
  `0x40` between trainings while its fine partner moves with it; mixing
  lifetimes could pair values that no training produced.
- **Mode registers:** MR14 and MR15 from that capture's parameter words 50
  and 51, through boot0's own `dram_tpr1` bit 31 path. MR12 is not captured.

## Implemented RAM-only preparation

1. **Record builder:** `tools/training-record.py from-capture`, turning a
   capture directory and the set file into the record format the design
   specifies (chip ID, boot0 checksum, strap CRC, temperature fields left
   as "unknown", registers as runs, masks, MR14/MR15).
2. **Restore wrapper variant:** `tools/prepare-ddr.py --variant restore`.
   It uploads the record to SRAM, applies the P-state-0-only and
   initialization-only parameter edits plus `dram_tpr1` bit 31 and words
   32–51, redirects the call at `0x56142` to a table-driven writer
   (`MicroContMuxSel` cleared, writes in record order, set to 1, then
   `0x4bca8`), and after `init_DRAM` reads every restored register back and
   runs quick-check part A below 4 GiB. It reports each step in the
   mailbox. On any failure it stops and reports; this first test makes no
   in-place retraining, because retraining within a lifetime has never run
   on these boards.
3. **Host checks:** the wrapper in Unicorn with the patched boot0 calls
   modeled (edit order, the record checks, the writer's accesses, every
   refusal), and `run-ddr.py` with a modeled FEL device for the restore mode.

## Host evidence and SRAM bounds

The evidence directory is
`~/.cache/sunstoneos/evidence/resume-dram-restore-1025/`.
The restore build is `build-final`; `from-capture` contains the two
board records and their producer checks. No check opens a physical device. Each final check directory preserves its
checker sources and their hashes under `checker-sources/`.

| Check | Result and scope |
| --- | --- |
| `checks/training-record-capture.py` | Both recorded P-state-0 references produce 13,836-byte records; seven altered identity/data cases refuse. Parameter words 32–51 retain all 32 bits. |
| `checks/ddr-wrapper.py` with restore build | Seventeen instruction cases pass, including the actual patched Thumb BLX, full-width parameters, wrong SID, header/payload CRC, malformed sections, arbitrary address/mask rejection, size, missing hook, two readback failures and sampled-memory failure. boot0 calls and MMIO effects are modeled. |
| `checks/ddr-host-retention.py` | Nineteen cases pass. Restore errors keep ownership and prevent the host's RAM-page transactions; record/helper upload mismatches stop before DDR execution. |
| `checks/training-record.py` | Existing record and card-image plan checks pass without device access. |
| Existing P-state-0 wrapper | Its focused instruction check passes; the 432-byte wrapper remains byte-identical to the accepted capture build. |

The nominal instruction case executes 2,345,249 instructions under a
4,000,000-instruction fixture limit. It performs 1,016 masked PHY writes,
immediate readback and a second readback pass after modeled `init_DRAM`.
The sample touches two 4 KiB pages at `0x41000000` and `0x42000000`.
Thirty-two walking-one passes and two address-pattern passes each write
and read 278,528 bytes in total. This bounds software work and detects
gross errors; it does not cover every channel, rank or the full memory map.

| Artifact or reservation | Address and size |
| --- | --- |
| Wrapper | `0x85000`, 504 bytes; limit 4 KiB |
| Mailbox | `0x86000`, 128 bytes |
| Record | `0x87000`, 13,836 bytes; reservation 24 KiB |
| FEL head shadow | `0x8e000`, 12 KiB |
| Helper and address/mask table | `0x92000`, 9,864 bytes; reservation 32 KiB |
| Private stack | `0x9b000..0x9f000`, 16 KiB |

The build manifest gives source and payload SHA-256 identities and the
linked allocated sections. The helper has one allocated code/data section
and no uninitialized BSS. The helper digest is
`65b20a53e13fa3d484343869bfee78f4fe503d15b6cecf78f5eeef92c28ff080`.
The port 3-3 record digest is
`ae1da9e29fccfd68d8d99f1bd2ab880228978038992dcb69b3efd88c57dab7e4`;
the port 3-1 record digest is
`7a9f0213da86318d972e1f366d2cc446e53b4a7dd7f1974fae3c0f2e49372793`.
These records use generation 1, boot0 checksum `0x2fce7bb3`, parameter-set
index 5 and strap CRC `0xf874a6a5`. Unknown temperature is encoded as
`INT32_MIN` with limit zero; this experiment has no temperature admission
measurement and remains limited to supervised room-temperature cold starts.

Admission failures return error 1 (geometry), 2 (header CRC), 3 (payload
CRC), 4 (SID), 5 (boot0) or 6 (strap) before parameter edits.
Runtime failures return 7 (PHY readback), 8 (size), 9 (sampled memory),
10 (missing or repeated hook) or 11 (cache/MMU state after boot0).
A readback failure inside boot0 restores the wrapper context from SRAM,
swaps the FEL head back and returns without resuming boot0. The runner
requires success, 1,016 writes, one hook call, 278,528 sampled read bytes
and the 12,288 MiB size before issuing its RAM-page checks.

## Commands for one board after approval

In a fresh lifetime after a power removal, at room temperature after at
least one hour unpowered, matching the captures:

1. Restore DDR start (`rs-ddr`): `run-ddr.py` with the restore build and
   the board's record through `--record`. The build must use
   `--variant restore --restore-set` with the fixed analysis file.
2. The v5 capture (`rs-capture-1`) with `--ddr-evidence $S/rs-ddr`.
3. The [memory qualification image](../memory-qualification.md), March C-
   over the 12,288 MiB map, three to five runs.

## Predictions

| Item | Prediction |
| --- | --- |
| `rs-ddr/firmware.log` | `Training result is = 7` about 36 ms after `DRAM BOOT DRIVE INFO`; `Actual DRAM SIZE =12288 M` about 40 ms after it; `DRAM simple test OK.` |
| Restore mailbox | Every record check passes, the read-back of all restored registers matches, part A passes |
| Both 4 KiB pages of `run-ddr.py` | Read back |
| `rs-capture-1` against the record | Every restored register equal to the record; the other registers equal the initialization-only capture or within the P-state-0-only lifetime spread |
| March C- | Pass over the full map |
| DRAM interval | About 60 ms with the quick check, against 4,437–4,451 ms for P-state-0-only training |

A failure at any step stops the board's session, as every runner does
today, and the board needs a power removal.

## Qualifying the restore later

The first test shows that one restored boot works. Qualification, once
power cycles are possible, adds per board:

1. Ten restored lifetimes at room temperature, each with March C-.
2. Restored lifetimes after cold and warm starts, each with March C- and a
   stress run; these also give the temperature limit the record needs.
3. The fallback cases of the design: a wrong record CRC, the other board's
   record, a changed strap CRC and a forced read-back failure, with identity and CRC failures
   refusing before parameter edits and the readback failure stopping before
   further DRAM use.
4. An in-place retraining after a refused restore, once a single run shows
   that `init_DRAM` can train twice in one lifetime.
5. The card path only after these pass, under
   [docs/goal.md](../goal.md) section 4.

## Confidence and residual risk

- **High confidence** that the 976 registers in both boards' diffs are
  training state: they differ between trained and untrained PHYs on two
  chips. The other 60 are in one board's diff only and are restored as
  harmless on the other, where they hold their initialization value.
- **Status bits that flip rarely:** status is decided from two captures
  per lifetime, and `0x0a51022c` changed in only one of ten lifetimes. A
  register that flips as rarely may still be in the set. Its read-back
  right after the write could then mismatch and stop the test, which costs
  a lifetime but no safety.
- **Registers the set may miss:**
  - a trained register that equals its initialization value in every
    captured lifetime on both boards (2–4% of the set match that value on
    one board, so about two of 1,036 could be missed on both);
  - registers outside the captured address ranges, which follow boot0's
    own accesses (`0x0000`–`0xbfff` read zero);
  - the `0xa0000` block, whose words change between power-cycled
    lifetimes and whose function is unknown;
  - MR12 and any mode register that the message block does not carry.
  The memory test is the check for all of these.
- **The reference path has no full memory test.** P-state-0-only training
  has passed boot0's simple test and two 4 KiB pages per lifetime, never
  March C-. The restore inherits that path, so qualification needs a
  March C- baseline after a P-state-0-only training as well as after the
  standard four-P-state one.
- **Temperature:** every capture is a room-temperature cold start. Until
  cold and warm lifetimes exist, a record is valid only for room-temperature
  cold starts, and the temperature gate needs a THS reading that is not
  built yet.
- **Spread:** two P-state-0-only lifetimes per board give a rough spread
  (median 2 codes, maximum 13 on the saved Vref bytes and a coarse step of
  `0x40` on a few byte-lane delays); the retraining interval waits for more
  data.
