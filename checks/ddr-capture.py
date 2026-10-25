"""Check the DRAM capture reader's register accesses against its stage tables.

The reader and tables come from tools/prepare-ddr-capture.py. Unicorn runs
the actual reader instructions on a modeled register space: every stage must
read each listed register once, with its width and in table order; write only
MicroContMuxSel and UcclkHclkEnables, restoring both; and publish the
completion word last. Refusal cases cover a wrong magic, a bad width and a
zero count. This models the instructions only, not the PHY's response to the
reads.
"""
import argparse
import json
from pathlib import Path
import struct
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R4,
                               UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                               UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_SP)

MUXSEL, UCCLK = 0x0aaa0000, 0x0aa80100
DONE = 0x454e4f44
RETURN = 0xf0000
SAVED = (UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
         UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)


def pattern(address, width):
    value = (address * 2654435761 >> 7) & 0xffffffff
    return value & 0xffff if width == 2 else value


def run(build, table, entries):
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.mem_map(0x40000, 0xc0000)
    cpu.mem_map(0x0a000000, 0x01000000)
    cpu.mem_map(0x43000000, 0x00200000)
    cpu.mem_write(0x85000, (build / "ddr-capture.bin").read_bytes())
    cpu.mem_write(0x43000000, table)
    for entry in entries:
        for index in range(entry["count"]):
            address = entry["address"] + entry["width"] * index
            fmt = "<H" if entry["width"] == 2 else "<I"
            cpu.mem_write(address, struct.pack(fmt, pattern(address, entry["width"])))
    cpu.mem_write(MUXSEL, struct.pack("<H", 1))
    cpu.mem_write(UCCLK, struct.pack("<H", 2))
    cpu.reg_write(UC_ARM_REG_CPSR, 0x60000053)
    cpu.reg_write(UC_ARM_REG_SP, 0xf3000)
    cpu.reg_write(UC_ARM_REG_LR, RETURN)
    for index, reg in enumerate(SAVED):
        cpu.reg_write(reg, 0x12340000 + index)
    reads, writes, other = [], [], []
    output = struct.unpack_from("<I", table, 12)[0]

    def on_read(uc, access, address, size, value, data):
        if 0x0a000000 <= address < 0x0b000000:
            reads.append((address, size))

    def on_write(uc, access, address, size, value, data):
        if 0x0a000000 <= address < 0x0b000000:
            writes.append((address, size, value & ((1 << 8 * size) - 1)))
        elif 0x43000010 <= address < 0x43000020 or output <= address < output + 0x40000:
            pass
        elif 0xf2000 <= address < 0xf3000:
            pass                                   # Stack below the entry SP.
        else:
            other.append(hex(address))

    cpu.hook_add(UC_HOOK_MEM_READ, on_read)
    cpu.hook_add(UC_HOOK_MEM_WRITE, on_write)
    cpu.emu_start(0x85000, RETURN, count=2_000_000)
    assert cpu.reg_read(UC_ARM_REG_PC) == RETURN
    assert cpu.reg_read(UC_ARM_REG_SP) == 0xf3000
    assert cpu.reg_read(UC_ARM_REG_CPSR) == 0x60000053
    for index, reg in enumerate(SAVED):
        assert cpu.reg_read(reg) == 0x12340000 + index
    assert not other, other
    header = struct.unpack("<8I", cpu.mem_read(0x43000000, 32))
    final = (struct.unpack("<H", cpu.mem_read(MUXSEL, 2))[0],
             struct.unpack("<H", cpu.mem_read(UCCLK, 2))[0])
    return cpu, header, reads, writes, final


def expected_accesses(entries, stop=None):
    reads, writes = [], []
    muxsel = ucclk = False
    for position, entry in enumerate(entries):
        if stop is not None and position == stop:
            break
        if entry["phy"] and not muxsel:
            reads += [(MUXSEL, 2), (UCCLK, 2)]
            writes.append((MUXSEL, 2, 0))
            muxsel = True
        if entry["ucclk"] and not ucclk:
            writes.append((UCCLK, 2, 3))
            ucclk = True
        reads += [(entry["address"] + entry["width"] * index, entry["width"])
                  for index in range(entry["count"])]
    if ucclk:
        writes.append((UCCLK, 2, 2))
    if muxsel:
        writes.append((MUXSEL, 2, 1))
    return reads, writes, muxsel


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    args = parser.parse_args()
    manifest = json.loads((args.build / "manifest.json").read_text())
    total = 0
    for stage in manifest["stages"]:
        table = (args.build / stage["file"]).read_bytes()
        entries = stage["entries"]
        assert struct.unpack_from("<4I", table) == (0x50414344, 1, len(entries), stage["output"])
        for position, entry in enumerate(entries):
            flags = struct.unpack_from("<4I", table, 32 + 16 * position)
            assert flags[:2] == (entry["address"], entry["count"]) and flags[3] == 0
        cpu, header, reads, writes, final = run(args.build, table, entries)
        want_reads, want_writes, phy = expected_accesses(entries)
        assert reads == want_reads, stage["name"]
        assert writes == want_writes, (stage["name"], writes)
        assert final == (1, 2)
        assert header[4] == len(entries) - 1 and header[7] == DONE
        assert header[5:7] == ((1, 2) if phy else (0xffffffff, 0xffffffff))
        # The two controls read back the values the reader wrote into them.
        written = {MUXSEL: 0, UCCLK: 3 if any(entry["ucclk"] for entry in entries) else 2}
        for entry in entries:
            data = cpu.mem_read(stage["output"] + entry["offset"], entry["count"] * entry["width"])
            fmt = "<%d%s" % (entry["count"], "H" if entry["width"] == 2 else "I")
            addresses = [entry["address"] + entry["width"] * index for index in range(entry["count"])]
            assert list(struct.unpack(fmt, data)) == [
                written.get(address, pattern(address, entry["width"])) for address in addresses]
        total += len(want_reads)
        print(f"{stage['name']}: {len(entries)} entries, {len(want_reads)} reads, "
              f"{len(want_writes)} control writes, {stage['output_bytes']} output bytes")

    phy_stage = next(stage for stage in manifest["stages"] if stage["name"] == "phy-blocks")
    base = bytearray((args.build / phy_stage["file"]).read_bytes())
    entries = phy_stage["entries"]
    # Wrong magic: no register access and no header write.
    table = bytearray(base)
    table[0] ^= 1
    _, header, reads, writes, final = run(args.build, bytes(table), entries)
    assert not reads and not writes and header[4] == 0xffffffff and header[7] == 0
    # A bad width or a zero count in entry 2 refuses at that entry, restores
    # both controls and leaves the completion word clear.
    for word, value in ((2, 3), (1, 0)):
        table = bytearray(base)
        struct.pack_into("<I", table, 32 + 16 * 2 + 4 * word, value)
        _, header, reads, writes, final = run(args.build, bytes(table), entries)
        want_reads, want_writes, _ = expected_accesses(entries, stop=2)
        assert reads == want_reads and writes == want_writes and final == (1, 2)
        assert header[4] == 2 and header[7] == 0
    print(f"PASS: {len(manifest['stages'])} stages, {total} register reads in table order; "
          "wrong magic, bad width and zero count refuse")


if __name__ == "__main__":
    main()
