"""Model of the AArch32 SRAM launcher that admits a prepared supervised session.

The launcher runs on CPU0 in AArch32 from SRAM at 0x52000. It checks the
session at 0x41f00000 and the request at 0x52c00 against the contract at
0x42004000. On admission it publishes the contract's deadlines, writes the
AArch64 entry address to RVBAR and requests a warm reset into AArch64; on
refusal it writes a report and returns with every register restored. The
model executes the actual launcher instructions on a Cortex-A15 and supplies
the system registers, the counter and the watchdog, CPU-control and RVBAR
registers. It stops at the reset request or at the return.

Per-device checks call check_bundle for a prepared bundle, or execute and
verify for launchers they generate themselves.
"""
import hashlib
import json
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_A15, UC_ARM_REG_R0, UC_ARM_REG_PC, UC_ARM_REG_CPSR,
                               UC_ARM_REG_SP, UC_ARM_REG_LR)

SESSION, CONTRACT, REQUEST, REPORT = 0x41f00000, 0x42004000, 0x52c00, 0x52d00
MODE, CONFIG, FLAG, ALT, RVBAR = 0x02050014, 0x02050010, 0x070501c0, 0x070501d4, 0x08001004
ADMITTED = 12
REFUSAL_MAGIC = 0x4153373352454631
COUNTER_HZ = 24000000
# MRC reads of SCTLR, MPIDR, CNTFRQ and RMR, by encoding with the destination
# register masked.
SYSTEM = {0xee110f10: 0, 0xee100fb0: 0x81000000, 0xee1e0f10: COUNTER_HZ, 0xee1c0f50: 0}


def load_segments(image):
    """(address, bytes) of each PT_LOAD segment of an ELF file."""
    with Path(image).open('rb') as stream:
        elf = ELFFile(stream)
        return [(int(s['p_vaddr']), s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']


class Launch:
    """The outcome of one launcher execution."""

    def __init__(self, name, contract_size):
        self.name = name
        self.contract_size = contract_size
        self.reads = []
        self.writes = []
        self.ticks = []
        self.stop = []


def execute(segments, code, request, contract, session, contract_magic, contract_size, name):
    """Run the launcher once and return its Launch.

    The memory hooks refuse any access outside the report, stack, contract
    deadline words and the modeled registers, and RVBAR writes before the
    contract carries contract_magic.
    """
    launch = Launch(name, contract_size)
    cpu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    cpu.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
    for base, size in [(0x50000, 0x30000), (0x41000000, 0x1200000), (0x02050000, 4096),
                       (0x07050000, 4096), (0x08001000, 4096)]:
        cpu.mem_map(base, size)
    for address, data in segments:
        if data:
            cpu.mem_write(address, data)
    for address, data in [(0x52000, code), (REQUEST, request), (CONTRACT, contract), (SESSION, bytes(session))]:
        cpu.mem_write(address, bytes(data))
    cpu.mem_write(0x52ff0, b'A' * 16)
    cpu.reg_write(UC_ARM_REG_CPSR, 0xa80f0053)
    saved = {UC_ARM_REG_R0 + i: 0x12340000 + i * 0x101 for i in range(13)}
    saved.update({UC_ARM_REG_SP: 0x70000, UC_ARM_REG_LR: 0x60000, UC_ARM_REG_CPSR: cpu.reg_read(UC_ARM_REG_CPSR)})
    for register, value in saved.items():
        cpu.reg_write(register, value)
    mmio = {MODE: 0, CONFIG: 0, FLAG: 0, ALT: 0, RVBAR: 0, RVBAR + 4: 0}

    def read(uc, access, address, size, value, unused):
        if address < 0x02000000 or address >= 0x41000000:
            return
        assert address in mmio and size == 4, (name, hex(address), size)
        launch.reads.append(address)
        uc.mem_write(address, struct.pack('<I', mmio[address]))

    def write(uc, access, address, size, value, unused):
        if address < 0x02000000:
            assert REPORT <= address < REPORT + 128 or 0x53000 <= address < 0x54000 or 0x70000 - 60 <= address < 0x70000
        elif address >= 0x41000000:
            assert CONTRACT <= address < CONTRACT + 8 or CONTRACT + 80 <= address < CONTRACT + 112
        else:
            assert address in (RVBAR, RVBAR + 4) and size == 4
            assert struct.unpack('<Q', uc.mem_read(CONTRACT, 8))[0] == contract_magic
            launch.writes.append((address, value))
            mmio[address] = value

    def instruction(uc, address, size, unused):
        if address == 0x60000:
            launch.stop.append('return')
            uc.emu_stop()
            return
        word = struct.unpack('<I', uc.mem_read(address, 4))[0]
        if word & 0xffff0fff in SYSTEM:
            uc.reg_write(UC_ARM_REG_R0 + ((word >> 12) & 15), SYSTEM[word & 0xffff0fff])
            uc.reg_write(UC_ARM_REG_PC, address + 4)
        elif word & 0xfff00fff == 0xec500f0e:
            # MRRC of the physical counter: successive reads advance by 100.
            value = 100000000 + 100 * len(launch.ticks)
            launch.ticks.append(value)
            uc.reg_write(UC_ARM_REG_R0 + ((word >> 12) & 15), value & 0xffffffff)
            uc.reg_write(UC_ARM_REG_R0 + ((word >> 16) & 15), value >> 32)
            uc.reg_write(UC_ARM_REG_PC, address + 4)
        elif word == 0xee0c0f50:
            # MCR to RMR: the warm-reset request into AArch64.
            assert uc.reg_read(UC_ARM_REG_R0) == 3 and mmio[RVBAR] == 0x41000000 and mmio[RVBAR + 4] == 0
            launch.stop.append('handoff')
            uc.emu_stop()
        elif word == 0xe320f003:
            raise AssertionError(name + ' unexpectedly parked')

    cpu.hook_add(UC_HOOK_MEM_READ, read)
    cpu.hook_add(UC_HOOK_MEM_WRITE, write)
    cpu.hook_add(UC_HOOK_CODE, instruction)
    cpu.emu_start(0x52000, 0x52c00, count=20000)
    launch.report = struct.unpack('<16Q', cpu.mem_read(REPORT, 128))
    launch.published = bytes(cpu.mem_read(CONTRACT, contract_size))
    launch.session_intact = bytes(cpu.mem_read(SESSION, len(session))) == bytes(session)
    launch.canary_intact = bytes(cpu.mem_read(0x52ff0, 16)) == b'A' * 16
    launch.registers = [(register, value, cpu.reg_read(register)) for register, value in saved.items()]
    return launch


def verify(launch, reason, contract, contract_magic, deadline_seconds, identity):
    """Require the expected outcome of one launch.

    Admission must publish the contract with its start, one-second admission
    deadline and deadline_seconds supervision deadlines from the first counter
    read. A refusal must leave the contract untouched, report the session
    identity and restore every register.
    """
    size = launch.contract_size
    report, published = launch.report, launch.published
    assert report[0] == reason, (launch.name, report[0], launch.stop)
    assert launch.session_intact and launch.canary_intact
    if reason == ADMITTED:
        ticks = launch.ticks
        deadline = deadline_seconds * COUNTER_HZ
        assert launch.stop == ['handoff'] and launch.writes == [(RVBAR, 0x41000000), (RVBAR + 4, 0)] and len(ticks) == 4
        assert struct.unpack_from('<Q', published)[0] == contract_magic
        assert struct.unpack_from('<4Q', published, 80) == (ticks[0], ticks[0] + COUNTER_HZ, ticks[0] + deadline, ticks[0] + deadline)
        assert published[8:80] == contract[8:80] and published[112:] == contract[112:size]
    else:
        assert launch.stop == ['return'] and not launch.writes and not launch.ticks and published == contract[:size]
        assert report[1:7] == (0, 0, *identity, REFUSAL_MAGIC) and not any(report[7:])
        for register, value, final in launch.registers:
            assert final == value, (launch.name, register)


def check_bundle(bundle, output, session, contract, deadline, cases, scope, message,
                 image_name='image.elf', header_offset=592, padded_session=False, runtime_contract=False):
    """Check a prepared bundle's launcher against its own session template.

    session is the (magic, version, size) header the template must carry at
    header_offset, and contract the (magic, size) of the contract. A padded
    session file may be a zero-filled 4096-byte page. cases lists (name,
    reason, session_patch, request_patch) tuples, where a patch is an (offset,
    bytes) pair or None; cases may also be a function of the template that
    returns such a list. With runtime_contract, the admitted contract is
    written to runtime-contract.bin. Return the result record.
    """
    output.mkdir(parents=True, exist_ok=False)
    session_magic, session_version, session_size = session
    contract_magic, contract_size = contract
    image = bundle / image_name
    segments = load_segments(image)
    code = (bundle / 'launch.bin').read_bytes()
    assert 0 < len(code) <= 0xc00
    template = (bundle / 'session.bin').read_bytes()
    if padded_session:
        assert len(template) in (session_size, 4096) and not any(template[session_size:])
        template = template[:session_size]
    contract_page = (bundle / 'contract.bin').read_bytes()
    assert len(contract_page) == 4096 and not any(contract_page[contract_size:])
    contract_bytes = contract_page[:contract_size]
    request = (bundle / 'request.bin').read_bytes()
    assert len(template) == session_size and len(contract_bytes) == contract_size
    assert struct.unpack_from('<3Q', template, header_offset) == (session_magic, session_version, session_size)
    identity = struct.unpack_from('<3Q', template, 24)
    results = []
    for name, reason, session_patch, request_patch in (cases(template) if callable(cases) else cases):
        case_session = bytearray(template)
        case_request = bytearray(request)
        for data, patch in ((case_session, session_patch), (case_request, request_patch)):
            if patch is not None:
                offset, value = patch
                data[offset:offset + len(value)] = value
        launch = execute(segments, code, case_request, contract_bytes, case_session, contract_magic, contract_size, name)
        verify(launch, reason, contract_bytes, contract_magic, deadline, identity)
        if reason == ADMITTED and runtime_contract:
            (output / 'runtime-contract.bin').write_bytes(launch.published)
        results.append(dict(case=name, reason=reason, terminal=launch.stop[0], mmio_writes=launch.writes,
                            counter_reads=launch.ticks))
    result = dict(passed=True, physical_access=False, cases=results,
                  image_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                  launcher_sha256=hashlib.sha256(code).hexdigest(),
                  session_sha256=hashlib.sha256(template).hexdigest(), scope=scope)
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(message.format(count=len(results)))
    return result
