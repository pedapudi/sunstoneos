"""Check wrapper control flow and FEL preservation with simulated blob calls.

This checks the wrapper's assembly only. It does not simulate the DDR hardware,
PMIC, or the blob functions replaced by the hooks below. The CPU PLL block is
modelled (checks/ddr_clock_model.py): after boot0's board setup the wrapper
must return CPU-L and the DSU to the ROM's words in the CPU clock driver's
order and waits, leave CPU-B alone, survive a counter wrap and a lock that
drops once, and report in mailbox word 15 a lock or update timeout, a
disabled PLL, a mux that does not park, a stopped counter (with the poll
limit lowered to 2,000) and a PLL already at the ROM point.
"""
import argparse
from pathlib import Path
import struct
import runpy
import sys
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import *

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("build", type=Path)
parser.add_argument("--record", type=Path)
parser.add_argument("--other-record", type=Path)
parser.add_argument("--output", type=Path)
args = parser.parse_args()
import json
sys.path.insert(0, str(Path(__file__).parent))
from ddr_clock_model import BOOT0, ROM, ClockModel
manifest = json.loads((args.build / "manifest.json").read_text())
# A capture variant patches dram_tpr13 bit 2 and dram_tpr10 bits 28:16 after
# the copy (tools/prepare-ddr.py --variant); the default build patches nothing.
variant = json.loads((args.build / "manifest.json").read_text()).get("variant")
if variant == "restore":
    if not args.record or not args.other_record or not args.output:
        parser.error("restore checks require --record, --other-record and --output")
    sys.argv = [str(Path(__file__).with_name("ddr-restore.py")), str(args.build),
                "--record", str(args.record), "--other-record", str(args.other_record), "--output", str(args.output)]
    runpy.run_path(str(Path(__file__).with_name("ddr-restore.py")), run_name="__main__")
    sys.exit(0)

# (board setup result, init_DRAM result, clock case, expected word 15)
CASES = [(0, 4096, "nominal", 0), (0, 0, "nominal", 0), (0xffffffff, 0, "nominal", 0),
         (0, 4096, "cpu-l-lock-timeout", 0x103), (0, 4096, "dsu-update-timeout", 0x302),
         (0, 4096, "cpu-l-disabled", 0x101), (0, 4096, "at-rom-point", 0),
         (0, 4096, "counter-wraps", 0), (0, 4096, "dsu-lock-glitch", 0),
         (0, 4096, "cpu-l-mux-stuck", 0x105), (0, 4096, "counter-stopped", 0x104),
         (0, 4096, "update-counter-stops", 0x104), (0, 4096, "lock-counter-stops", 0x104)]
NOMINAL = ("nominal", "counter-wraps", "dsu-lock-glitch")
STOPS = ("counter-stopped", "update-counter-stops", "lock-counter-stops")
clock_results = []
for board_result, dram_result, clock_case, clock_word in CASES:
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.mem_map(0x40000, 0xc0000)
    original_fel = bytes(range(256)) * 48
    cpu.mem_write(0x47000, original_fel)
    cpu.mem_write(0x8e000, (args.build / "boot0-head.bin").read_bytes())
    cpu.mem_write(0x4a000, (args.build / "boot0-body.bin").read_bytes())
    cpu.mem_write(0x85000, (args.build / "ddr-entry.bin").read_bytes())
    cpu.reg_write(UC_ARM_REG_CPSR, 0x60000053)
    cpu.reg_write(UC_ARM_REG_SP, 0xf3000)
    cpu.reg_write(UC_ARM_REG_LR, 0xf0000)
    registers = (UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
                 UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)
    for index, reg in enumerate(registers):
        cpu.reg_write(reg, 0x12340000 + index)
    words = list(range(1800, 1832))
    words[27], words[30] = 0xb25f0000, 0x10065
    selected = struct.pack("<32I", *words)
    expected = list(words)
    if variant:
        expected[30] &= ~4
    if variant == "init-only":
        expected[27] = (expected[27] & ~0x1fff0000) | 0x00010000
    expected = struct.pack("<32I", *expected)
    called = []

    def call(uc, address, size, data):
        if address not in (0x49898, 0x48fac, 0x49bdc):
            # The wrapper must not enter any other blob path.
            assert not 0x47000 <= address < 0x83000, hex(address)
            return
        called.append(address)
        if address == 0x48fac:
            uc.mem_write(0x47038, selected)
            result = board_result
        elif address == 0x49bdc:
            assert uc.reg_read(UC_ARM_REG_R0) == 0
            assert uc.reg_read(UC_ARM_REG_R1) == 0x831f8
            assert bytes(uc.mem_read(0x831f8, 128)) == expected
            result = dram_result
        else:
            result = 0
        uc.reg_write(UC_ARM_REG_R0, result)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def protect(uc, access, address, size, value, data):
        if 0x47000 <= address < 0x4a000:
            assert uc.reg_read(UC_ARM_REG_CPSR) & 0xc0 == 0xc0

    words = dict(BOOT0)
    if clock_case == "cpu-l-disabled":
        words[0x08871000] &= ~0x80000000
    if clock_case == "at-rom-point":
        words.update(ROM)
    if clock_case in STOPS:
        cpu.mem_write(manifest["clock_poll_limit"], struct.pack("<I", 2000))
    clocks = ClockModel(
        cpu, manifest["clock_ticks"], words=words,
        stuck_lock=("cpu-l",) if clock_case in ("cpu-l-lock-timeout", "lock-counter-stops") else (),
        stuck_update=("dsu",) if clock_case == "dsu-update-timeout" else
        ("cpu-l",) if clock_case == "update-counter-stops" else (),
        lock_glitch=("dsu",) if clock_case == "dsu-lock-glitch" else (),
        stuck_mux=("cpu-l",) if clock_case == "cpu-l-mux-stuck" else (),
        start=0xffffff00 if clock_case == "counter-wraps" else 0x10000,
        step=0 if clock_case == "counter-stopped" else 4,
        # Stops after the 20 us park wait, inside the update or lock wait.
        stop=0x10000 + 600 if clock_case in STOPS[1:] else None)
    cpu.hook_add(UC_HOOK_CODE, call)
    cpu.hook_add(UC_HOOK_MEM_WRITE, protect)
    cpu.emu_start(0x85000, 0xf0000, count=20000000)
    assert cpu.reg_read(UC_ARM_REG_PC) == 0xf0000
    assert cpu.reg_read(UC_ARM_REG_SP) == 0xf3000
    assert cpu.reg_read(UC_ARM_REG_CPSR) == 0x60000053
    for index, reg in enumerate(registers):
        assert cpu.reg_read(reg) == 0x12340000 + index
    assert bytes(cpu.mem_read(0x47000, 0x3000)) == original_fel
    result = struct.unpack("<4I", cpu.mem_read(0x86000, 16))
    assert result == (0x53524444, 5, dram_result if not board_result else 0, board_result)
    assert called == [0x49898, 0x48fac] + ([0x49bdc] if not board_result else [])
    assert struct.unpack("<I", cpu.mem_read(0x8603c, 4))[0] == clock_word, (clock_case, hex(clock_word))
    assert not [a for a, v in clocks.writes() if a in (0x08872000, 0x0887201c)], "CPU-B written"
    w = clocks.words
    if clock_case in NOMINAL:
        cpu_l_select = clocks.check_sequence("cpu-l", 0x03010000)
        select = clocks.check_sequence("dsu", 0x03010105)
        dsu_park = next(t for op, a, v, t in clocks.log if op == "w" and a == 0x0887301c)
        assert dsu_park - cpu_l_select >= 480, "20 us after CPU-L selects its PLL"
        assert clocks.tick - select >= 480, "20 us after the last mux write"
        assert {a: w[a] for a in ROM} == ROM and w[0x08872000] == 0xf8802700 and w[0x0887201c] == 0x03000000
        # CPU-L finishes before the DSU starts.
        writes = clocks.writes()
        assert max(i for i, (a, _) in enumerate(writes) if a < 0x08872000) < min(
            i for i, (a, _) in enumerate(writes) if a >= 0x08873000)
    elif clock_case == "at-rom-point" or clock_case == "cpu-l-disabled":
        assert not clocks.writes() and w == words
    elif clock_case == "cpu-l-mux-stuck":
        # The park did not take: no PLL write, the DSU untouched.
        assert clocks.writes() == [(0x0887101c, 0)] and w == words
    elif clock_case in STOPS:
        # 2,000 polls, then code 4 with CPU-L parked; no further write, the DSU untouched.
        writes = clocks.writes()
        assert writes[0] == (0x0887101c, 0) and w[0x0887101c] == 0
        assert writes[1:] == ([] if clock_case == "counter-stopped" else
                              [(0x08871000, 0xf8802600), (0x08871000, 0xfc802600)]), writes
        # PLL reads after the update request, split by the update bit.
        reads = [e for e in clocks.log if e[0] == "r" and e[1] == 0x08871000][1:]
        updating = len([e for e in reads if e[2] & (1 << 26)])
        if clock_case == "update-counter-stops":
            assert updating == len(reads) == 2000, (updating, len(reads))
        elif clock_case == "lock-counter-stops":
            # The read that saw the update bit clear, then 2,000 lock polls.
            assert len(reads) - updating == 2001, (updating, len(reads))
    elif clock_case == "cpu-l-lock-timeout":
        # Stopped with CPU-L parked on the oscillator after 70 ms; the DSU untouched.
        assert clocks.writes()[-1] == (0x08871000, 0xfc802600) and w[0x0887101c] == 0
        assert not [a for a, v in clocks.writes() if a >= 0x08873000]
        last_read = max(t for op, a, v, t in clocks.log if a == 0x08871000)
        update_at = [t for op, a, v, t in clocks.log if op == "w" and v & (1 << 26)][0]
        assert 1680000 <= last_read - update_at < 1680000 + 1000
    elif clock_case == "dsu-update-timeout":
        assert {a: w[a] for a in (0x08871000, 0x0887101c)} == {0x08871000: 0xf8802600, 0x0887101c: 0x03010000}
        assert w[0x0887301c] == 0x00000105 and clocks.writes()[-1] == (0x08873000, 0xfc802600)
        last_read = max(t for op, a, v, t in clocks.log if a == 0x08873000)
        update_at = [t for op, a, v, t in clocks.log if op == "w" and a == 0x08873000 and v & (1 << 26)][0]
        assert 240000 <= last_read - update_at < 240000 + 1000
    clock_results.append(clock_case)

# Exercise the actual Thumb-to-ARM logging trampoline without the call hooks.
cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
cpu.mem_map(0x40000, 0xc0000)
cpu.mem_write(0x4a000, (args.build / "boot0-body.bin").read_bytes())
cpu.mem_write(0x85000, (args.build / "ddr-entry.bin").read_bytes())
cpu.reg_write(UC_ARM_REG_CPSR, 0x53)
cpu.reg_write(UC_ARM_REG_R0, ord("A"))
cpu.reg_write(UC_ARM_REG_LR, 0xf0000)
cpu.emu_start(0x4a541, 0xf0000, count=100)
assert cpu.mem_read(0xe0000, 1) == b"A"
assert struct.unpack("<I", cpu.mem_read(0x86018, 4))[0] == 1
print("PASS wrapper: success/failure returns, FEL state and registers restored, selected parameters copied, "
      "logging trampoline; CPU-L and DSU returned to the ROM point in the driver's order, CPU-B untouched, "
      "counter wrap and a lock glitch survived; lock and update timeouts, a disabled PLL, a mux that does not "
      "park, three stopped-counter points and an already-set point reported in word 15.")
