#!/usr/bin/env python3
"""Execute the pinned boot0 parser's cache wrappers with synthetic cache geometry.

System registers, cache effects and the delay are modeled. This checks instruction
ordering and set/way coverage, not physical publication or a complete boot.
"""
import argparse
import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM, CS_MODE_THUMB
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn import arm_const as arm


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot0", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    raw = args.boot0.read_bytes()
    identity = hashlib.sha256(raw).hexdigest()
    assert identity == "829ea8e3b65d71a02f2d0e094bc073de3a10869f948f62db49dbf023d07fabe3"
    # These are the actual parser's enable and successful-return disable calls.
    thumb = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    for address, target in ((0x5dca2, 0x49066), (0x5dcd4, 0x49074)):
        instruction = next(thumb.disasm(raw[address-0x47000:address-0x47000+4], address))
        assert (instruction.mnemonic, instruction.op_str) == ("bl", f"#{target:#x}")
    instructions = {}
    ranges = ((CS_MODE_ARM, 0x47b80, 0x47c1c),
              (CS_MODE_THUMB, 0x48dbc, 0x48df6),
              (CS_MODE_THUMB, 0x49066, 0x4907e))
    for mode, start, end in ranges:
        for instruction in Cs(CS_ARCH_ARM, mode).disasm(raw[start-0x47000:end-0x47000], start):
            instructions[instruction.address] = instruction
    args.output.mkdir(parents=True, exist_ok=False)
    cpu = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    cpu.mem_map(0x40000, 0x50000)
    cpu.mem_map(0x90000, 4096)
    cpu.mem_write(0x47000, raw)
    state = {"sctlr": 0x1001, "selector": 0}
    events, maintenance, returned = [], [], []
    # Synthetic two data-cache levels and one instruction-only level.
    geometry = {0: (2, 2), 2: (4, 4)}

    def skip(address, size):
        thumb_bit = 1 if cpu.reg_read(arm.UC_ARM_REG_CPSR) & 0x20 else 0
        cpu.reg_write(arm.UC_ARM_REG_PC, address + size | thumb_bit)

    def hook(u, address, size, unused):
        if address == 0x90000:
            returned.append(True)
            u.emu_stop()
            return
        if address == 0x48d80:
            events.append(["modeled-delay", u.reg_read(arm.UC_ARM_REG_R0)])
            u.reg_write(arm.UC_ARM_REG_PC, u.reg_read(arm.UC_ARM_REG_LR))
            return
        instruction = instructions.get(address)
        assert instruction is not None, hex(address)
        name, operands = instruction.mnemonic, instruction.op_str
        if name in ("dmb", "dsb", "isb"):
            events.append([name, operands])
        if name not in ("mrc", "mcr"):
            return
        if address in (0x48dbe, 0x48de0):
            register = arm.UC_ARM_REG_R4 if address == 0x48dbe else arm.UC_ARM_REG_R3
            u.reg_write(register, state["sctlr"])
        elif address in (0x48dcc, 0x48de8):
            register = arm.UC_ARM_REG_R4 if address == 0x48dcc else arm.UC_ARM_REG_R3
            state["sctlr"] = u.reg_read(register)
            events.append(["sctlr", state["sctlr"]])
        elif address == 0x47b84:
            u.reg_write(arm.UC_ARM_REG_R0, (3 << 24) | 3 | (4 << 3) | (1 << 6))
        elif address in (0x47bac, 0x47c0c):
            state["selector"] = u.reg_read(arm.UC_ARM_REG_R10)
            events.append(["selector", state["selector"]])
        elif address == 0x47bb4:
            sets, ways = geometry[state["selector"]]
            u.reg_write(arm.UC_ARM_REG_R1, 2 | ((ways-1) << 3) | ((sets-1) << 13))
        elif address == 0x47be8:
            value = u.reg_read(arm.UC_ARM_REG_R11)
            maintenance.append(value)
            events.append(["dccisw", value])
        else:
            raise AssertionError((hex(address), name, operands))
        skip(address, size)

    cpu.hook_add(UC_HOOK_CODE, hook)
    try:
        for entry, expected in ((0x49067, 0x1005), (0x49075, 0x1001)):
            cpu.reg_write(arm.UC_ARM_REG_SP, 0x8c000)
            cpu.reg_write(arm.UC_ARM_REG_LR, 0x90001)
            cpu.emu_start(entry, 0, count=10000)
            assert state["sctlr"] == expected
        assert len(returned) == 2
        expected = []
        for selector, (sets, ways) in geometry.items():
            shift = 32 - (ways-1).bit_length()
            expected.extend(selector | (way << shift) | (index << 6)
                            for way in reversed(range(ways))
                            for index in reversed(range(sets)))
        assert maintenance == expected and state["selector"] == 0
        disable = events.index(["sctlr", 0x1001])
        barrier = events.index(["dsb", "st"])
        assert max(i for i, event in enumerate(events) if event[0] == "dccisw") < barrier < disable
        result = {"passed": True, "boot0_sha256": identity, "cache_operations": len(maintenance),
                  "events": events, "physical_access": False, "scope": __doc__}
        (args.output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(f"PASS: {len(maintenance)} synthetic set/way operations before disable")
    except BaseException as error:
        (args.output / "failure.json").write_text(json.dumps(
            {"error": repr(error), "pc": hex(cpu.reg_read(arm.UC_ARM_REG_PC)),
             "state": state, "events": events}, indent=2) + "\n")
        raise


if __name__ == "__main__":
    main()
