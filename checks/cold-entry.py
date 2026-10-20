#!/usr/bin/env python3
"""Check actual boot0 header mutation/cleanup and the cold monitor instructions."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from elftools.elf.elffile import ELFFile
from unicorn import (Uc, UC_ARCH_ARM, UC_ARCH_ARM64, UC_MODE_THUMB, UC_MODE_ARM,
                     UC_HOOK_CODE, UC_HOOK_MEM_WRITE)
from unicorn import arm_const as arm, arm64_const as a64


def sha(data):
    return hashlib.sha256(data).hexdigest()


def inherited_payload(boot0, payload, output):
    assert sha(boot0) == "829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3"
    cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    for base, size in ((0x40000, 0x50000), (0x90000, 4096), (0x42e00000, 65536),
                       (0x47000000, 65536), (0x08001000, 4096)):
        cpu.mem_map(base, size)
    cpu.mem_write(0x47000, boot0)
    package = bytearray(1024 + len(payload))
    package[:14] = b"sunxi-package\0"
    struct.pack_into("<I", package, 0x20, 1)
    package[0x40:0x48] = b"monitor\0"
    struct.pack_into("<6I", package, 0x80, 1024, len(payload), 0, 3, 0, 0)
    package[1024:] = payload
    cpu.mem_write(0x42e00000, bytes(package))
    dram, identity = bytes(range(128)), bytes(range(0xa0, 0xb0))
    cpu.mem_write(0x831f8, dram)
    cpu.mem_write(0x470f0, b"BOOTDATA")
    cpu.mem_write(0x83c20, struct.pack("<I", 12288))
    cpu.mem_write(0x89100, identity)
    sp, pointers = 0x88000, 0x89000
    for index in range(4):
        cpu.reg_write(arm.UC_ARM_REG_R0 + index, pointers + index * 4)
    cpu.mem_write(sp, struct.pack("<3I", pointers + 16, pointers + 20, pointers + 24))
    cpu.reg_write(arm.UC_ARM_REG_SP, sp)
    cpu.reg_write(arm.UC_ARM_REG_LR, 0x90001)
    controls, ticks, stops = [], [], []
    state = {"sctlr": 0x30c51833, "rmr": 0x100}

    def hook(u, address, size, context):
        if address == 0x90000:
            stops.append("parser-return")
            u.emu_stop()
        elif address in (0x49066, 0x49074, 0x4993c):
            u.reg_write(arm.UC_ARM_REG_R0, 0)
            u.reg_write(arm.UC_ARM_REG_PC, u.reg_read(arm.UC_ARM_REG_LR))
        elif address == 0x4af58:
            u.reg_write(arm.UC_ARM_REG_R0, 0x89100)
            u.reg_write(arm.UC_ARM_REG_PC, u.reg_read(arm.UC_ARM_REG_LR))
        elif address == 0x48e8e:
            u.reg_write(arm.UC_ARM_REG_R4, state["sctlr"])
            u.reg_write(arm.UC_ARM_REG_PC, address + 4 | 1)
        elif address in (0x48d84, 0x48d94):
            value = 100000 + len(ticks) * 1500
            ticks.append(value)
            u.reg_write(arm.UC_ARM_REG_R6 if address == 0x48d84 else arm.UC_ARM_REG_R0, value)
            u.reg_write(arm.UC_ARM_REG_R1, 0)
            u.reg_write(arm.UC_ARM_REG_PC, address + 4 | 1)
        elif address in (0x48ea0, 0x48eaa, 0x48eae):
            value = u.reg_read(arm.UC_ARM_REG_R3)
            controls.append([hex(address), value])
            if address == 0x48ea0:
                state["sctlr"] = value
            u.reg_write(arm.UC_ARM_REG_PC, address + 4 | 1)
        elif address == 0x48ef0:
            u.reg_write(arm.UC_ARM_REG_R2, state["rmr"])
            u.reg_write(arm.UC_ARM_REG_PC, address + 4 | 1)
        elif address == 0x48efc:
            state["rmr"] = u.reg_read(arm.UC_ARM_REG_R2)
            u.reg_write(arm.UC_ARM_REG_PC, address + 4 | 1)
        elif address == 0x48f06:
            stops.append("reset-request")
            u.emu_stop()

    cpu.hook_add(UC_HOOK_CODE, hook)
    cpu.emu_start(0x5dc81, 0, count=200000)
    assert stops == ["parser-return"] and cpu.reg_read(arm.UC_ARM_REG_R0) == 0
    assert struct.unpack("<7I", cpu.mem_read(pointers, 28)) == (0, 0, 0x47000000, 0, 0, 0, 0)
    cpu.mem_write(sp + 0x1c, struct.pack("<6I", 0, 0, 0x47000000, 0, 0, 0))
    cpu.reg_write(arm.UC_ARM_REG_SP, sp)
    # This interval includes the actual cleanup and timer delay, unlike the
    # earlier handoff-only check. System operations remain modeled.
    cpu.emu_start(0x5e34d, 0, count=200000)
    assert stops == ["parser-return", "reset-request"]
    assert state == {"sctlr": 0x30c50830, "rmr": 0x103}
    assert controls == [["0x48ea0", 0x30c50830], ["0x48eaa", 0], ["0x48eae", 0]]
    assert struct.unpack("<2I", cpu.mem_read(0x08001004, 8)) == (0x47000000, 0)
    expected = bytearray(payload)
    expected[0x10:0x14] = bytes(4)
    expected[0x18:0x1c] = bytes(4)
    expected[0x24:0x2c] = b"BOOTDATA"
    expected[0x2c:0x30] = struct.pack("<I", 12288)
    expected[0x30:0xb0] = dram
    expected[0xb8:0xc8] = identity
    actual = bytes(cpu.mem_read(0x47000000, len(payload)))
    assert actual == expected
    (output / "inherited-monitor.bin").write_bytes(actual)
    return actual, {"sctlr_before": 0x30c51833, "sctlr_after": state["sctlr"],
                    "modeled_control_operations": controls, "counter_reads": ticks,
                    "scope": "Actual parser and main tail from 0x5e34c, including cleanup. No media, authentication, DDR, full boot0 initialization or physical reset."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--boot0", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    raw = (args.build / "monitor.bin").read_bytes()
    with (args.build / "cold-entry.elf").open("rb") as stream:
        elf = ELFFile(stream)
        symbols = {s.name: int(s["st_value"]) for s in elf.get_section_by_name(".symtab").iter_symbols()}
        assert elf["e_entry"] == symbols["a733_cold_image"] == 0x47000000
        assert symbols["a733_cold_report"] == 0x47008000
        assert symbols["a733_cold_vectors"] % 2048 == 0
    inherited, predecessor = inherited_payload(args.boot0.read_bytes(), raw, args.output)
    # Decode words independently so embedded data cannot truncate disassembly.
    # Only addresses actually executed by the processor model select a hook.
    disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    decoded = {}
    for offset in range(0, len(raw) - 3, 4):
        instruction = next(disassembler.disasm(raw[offset:offset + 4], 0x47000000 + offset), None)
        if instruction:
            decoded[instruction.address] = (instruction.mnemonic, instruction.op_str)
    defaults = {"currentel": 12, "mpidr_el1": 0x81000000, "midr_el1": 0x412fd050,
                "sctlr_el3": 0x30c50832, "scr_el3": 0x30, "cntfrq_el0": 0,
                "cntpct_el0": 24000123, "rvbar_el3": 0x47000000,
                "esr_el3": 0x96000010, "far_el3": 0xdead0000,
                "elr_el3": symbols["a733_cold_entry"], "spsr_el3": 0x3cd}
    cases = []

    def run(name, changes=None, mutate=None, status=None, vector=False):
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.mem_map(0x47000000, 65536)
        payload = bytearray(inherited)
        if mutate:
            mutate(payload)
        cpu.mem_write(0x47000000, bytes(payload))
        cpu.mem_write(0x47008000, b"\xa5" * 512)
        cpu.reg_write(a64.UC_ARM64_REG_SP, 0xdeadbeef)
        registers = dict(defaults, **(changes or {}))
        writes, controls, stopped = [], [], []

        def write(u, access, address, size, value, context):
            assert 0x47008000 <= address < address + size <= 0x47008200, (name, hex(address))
            writes.append([address, size, value])

        def instruction(u, address, size, context):
            if address == symbols["a733_cold_park"]:
                stopped.append(True)
                u.emu_stop()
                return
            mnemonic, operands = decoded[address]
            if mnemonic == "mrs":
                reg, system = operands.lower().split(", ")
                assert system in registers, system
                u.reg_write(getattr(a64, "UC_ARM64_REG_" + reg.upper()), registers[system])
                u.reg_write(a64.UC_ARM64_REG_PC, address + 4)
            elif mnemonic == "msr":
                system, value = operands.lower().split(", ")
                assert system in ("daifset", "vbar_el3"), system
                controls.append([system, value if value.startswith("#") else
                                 u.reg_read(getattr(a64, "UC_ARM64_REG_" + value.upper()))])
                u.reg_write(a64.UC_ARM64_REG_PC, address + 4)

        cpu.hook_add(UC_HOOK_MEM_WRITE, write)
        cpu.hook_add(UC_HOOK_CODE, instruction)
        start = symbols["a733_cold_vectors"] + 512 if vector else 0x47000000
        cpu.emu_start(start, 0, count=10000)
        assert stopped and cpu.reg_read(a64.UC_ARM64_REG_SP) == 0xdeadbeef
        report = bytes(cpu.mem_read(0x47008000, 512))
        assert bytes(cpu.mem_read(0x47000000, len(payload))) == payload
        if status is None:
            assert not writes and report == b"\xa5" * 512
            assert all(item[0] == "daifset" for item in controls)
        else:
            assert struct.unpack_from("<4Q", report) == (0x41533733434f4c44, 1, 512, status)
            assert writes[-1] == [0x47008000, 8, 0x41533733434f4c44]
            if vector:
                assert struct.unpack_from("<5Q", report, 128) == tuple(registers[k] for k in
                    ("esr_el3", "far_el3", "elr_el3", "spsr_el3")) + (4,)
            else:
                assert report[192:456] == payload[:264]
                assert struct.unpack_from("<9Q", report, 40) == tuple(registers[k] for k in
                    ("currentel", "midr_el1", "mpidr_el1", "sctlr_el3", "scr_el3",
                     "cntfrq_el0", "cntpct_el0", "rvbar_el3")) + (264,)
                assert controls[-1] == ["vbar_el3", symbols["a733_cold_vectors"]]
        (args.output / (name + ".bin")).write_bytes(report)
        cases.append({"case": name, "status": status, "report_writes": len(writes),
                      "control_writes": controls, "report_sha256": sha(report)})

    run("entry-capture", status=1)
    run("wrong-el", {"currentel": 4})
    run("wrong-core", {"mpidr_el1": 0x81000100})
    run("inherited-cache", {"sctlr_el3": defaults["sctlr_el3"] | 4})
    run("wrong-cpu", {"midr_el1": 0x414fd0b0})
    run("unexpected-optee", mutate=lambda p: struct.pack_into("<I", p, 0x18, 0x48000000), status=2)
    run("unpatched-loadword", mutate=lambda p: struct.pack_into("<I", p, 0x2c, 0x47000000), status=2)
    run("exception-record", status=3, vector=True)
    result = {"passed": True, "physical_access": False, "physical_boot_qualified": False,
              "monitor_sha256": sha(raw), "inherited_monitor_sha256": sha(inherited),
              "predecessor": predecessor, "cases": cases,
              "limitations": [
                  "System registers, counter progression and cache/reset effects are modeled. AArch64 inherited state is an independent fixture, not inferred from the AArch32 cleanup.",
                  "No remote-processor exclusion, memory ownership, peripheral access, FreeRTOS startup, recovery or physical storage operation is established.",
                  "The exception case enters an actual vector directly; it does not establish hardware exception delivery or recovery from an unresponsive memory transaction."]}
    (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("PASS boot0 cleanup/header integration and eight cold-entry instruction cases")


if __name__ == "__main__":
    main()
