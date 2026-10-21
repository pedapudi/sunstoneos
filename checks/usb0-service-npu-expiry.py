#!/usr/bin/env python3
"""Check that an NPU USB owner poll latches request expiry without hashing.

Runs the linked a733_observe_privileged() of NPU runtime, large-graph and
accelerator images under Unicorn. Provider, TinyUSB and failure entry points
return modeled values; npu_runtime and the observe identity are written
directly. An idle poll and a poll after an abandoned upload must call neither
npu_runtime_frame() nor a733_observe_digest(), and the abandoned upload must
still latch runtime reason 30 (expired request). No hardware is accessed."""
import argparse, hashlib, json, struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_LR, UC_ARM64_REG_SP, UC_ARM64_REG_PC

RETURN = 0x7fff0000
STACK = 0x7ff00000
NOW = 5_000_000
NR_IDLE, NR_UPLOAD, NR_FAILED = 0, 1, 7
RETURNS = {'npu_single_core_usb_poll': 1, 'a733_npu_application_observer_admit': 1, 'tusb_rhport_init': 1,
           'a733_usb_poll': 0, 'tud_task_ext': 0, 'tud_mounted': 0, 'a733_smp_counter': NOW,
           'a733_npu_application_fail': 0, 'accelerator_shared_live': 1, 'accelerator_shared_fail': 0,
           'a733_observe_deadline': 0}
COUNTED = ('npu_runtime_frame', 'a733_observe_digest', 'a733_npu_application_fail', 'accelerator_shared_fail')


def load(path):
    cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        loads = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD' and s['p_memsz']]
        merged = []
        for start, end in sorted((s['p_vaddr'] & ~0xfff, (s['p_vaddr'] + s['p_memsz'] + 0xfff) & ~0xfff) for s in loads):
            if merged and start <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], end)
            else:
                merged.append([start, end])
        for start, end in merged:
            cpu.mem_map(start, end - start)
        for segment in loads:
            cpu.mem_write(segment['p_vaddr'], segment.data())
        symbols = {}
        for s in elf.get_section_by_name('.symtab').iter_symbols():
            if s['st_info']['bind'] == 'STB_GLOBAL' and s.name:
                symbols[s.name] = s['st_value']
    cpu.mem_map(STACK, 0x20000)
    cpu.mem_map(RETURN, 0x1000)
    calls = {name: 0 for name in COUNTED}
    failure = []
    targets = {symbols[n]: n for n in list(RETURNS) + list(COUNTED) + ['a733_observe_failure'] if n in symbols}

    def hook(m, address, size, unused):
        name = targets.get(address)
        if name is None:
            return
        if name in calls:
            calls[name] += 1
        if name == 'a733_observe_failure':
            failure.append(m.reg_read(UC_ARM64_REG_X0))
            m.reg_write(UC_ARM64_REG_PC, RETURN)
        elif name in RETURNS:
            m.reg_write(UC_ARM64_REG_X0, RETURNS[name])
            m.reg_write(UC_ARM64_REG_PC, m.reg_read(UC_ARM64_REG_LR))

    cpu.hook_add(UC_HOOK_CODE, hook)
    return cpu, symbols, calls, failure


def poll(path, state, deadline):
    cpu, symbols, calls, failure = load(path)
    # Identity is live: armed before NOW, deadline far after it.
    cpu.mem_write(symbols['a733_observe_identity'] + 24, struct.pack('<QQ', 0, NOW * 100))
    cpu.mem_write(symbols['npu_runtime'], struct.pack('<II', state, 0))
    cpu.mem_write(symbols['npu_runtime'] + 16, struct.pack('<Q', deadline))
    cpu.reg_write(UC_ARM64_REG_SP, STACK + 0x1ff00)
    cpu.reg_write(UC_ARM64_REG_LR, RETURN)
    cpu.emu_start(symbols['a733_observe_privileged'], RETURN, count=2_000_000)
    assert cpu.reg_read(UC_ARM64_REG_PC) == RETURN, 'poll did not return'
    new_state, reason = struct.unpack('<II', bytes(cpu.mem_read(symbols['npu_runtime'], 8)))
    return dict(state=new_state, reason=reason, failure=failure, **calls)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', nargs=2, action='append', required=True, metavar=('NAME', 'ELF'))
    parser.add_argument('--baseline', nargs=2, action='append', default=[], metavar=('NAME', 'ELF'),
                        help='images built before the change; reported, not asserted')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    rows = []
    for name, path in args.image + args.baseline:
        current = (name, path) in map(tuple, args.image)
        idle = poll(path, NR_IDLE, 0)
        abandoned = poll(path, NR_UPLOAD, NOW - 1)
        row = dict(name=name, current=current, elf_sha256=hashlib.sha256(Path(path).read_bytes()).hexdigest(),
                   idle=idle, abandoned_upload=abandoned)
        rows.append(row)
        print(f"{'current' if current else 'baseline'} {name}: idle digests {idle['a733_observe_digest']}, "
              f"abandoned reason {abandoned['reason']} state {abandoned['state']} digests {abandoned['a733_observe_digest']}")
        if current:
            assert not idle['failure'] and not abandoned['failure'], row
            assert idle['reason'] == 0 and idle['state'] == NR_IDLE, row
            assert abandoned['reason'] == 30 and abandoned['state'] == NR_FAILED, row
            for case in (idle, abandoned):
                assert case['a733_observe_digest'] == 0 and case['npu_runtime_frame'] == 0, row
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(dict(passed=True, physical_access=False, images=rows), indent=2) + '\n')
    print('PASS: NPU owner polls latch expiry with no frame or digest work')


if __name__ == '__main__':
    main()
