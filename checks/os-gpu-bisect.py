#!/usr/bin/env python3
"""Linked GPU bisect limit and version-3 retirement over modeled service replies.

The image's real hook limit (Submit, Begin), retirement entry (os_gpu_retire),
CLOSE/PARK sequence, worker suspension, report and CPU-completion gate execute.
Service queues, the backend stop proof and the clocks are model boundaries, as
in checks/os-gpu-return.py. No GPU, firmware or scheduler executes.
"""
import argparse
import hashlib
import importlib.util
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_LR, UC_ARM64_REG_SP
import harness
from harness import Image, save

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--image', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=False)
types = {'board_application', 'a733_board_session', 'pvr_gpu_service', 'pvr_gpu_service_client', 'pvr_mimus_service',
         'a733_gpu_mimus', 'a733_gpu_platform', 'pvr_gpu_service_reply', 'pvr_gpu_service_request',
         'os_dispatch_record', 'Call'}
image = Image(a.image, types)
enums = {}
with a.image.open('rb') as f:
    elf = ELFFile(f)
    symbols = {s.name: s for s in elf.get_section_by_name('.symtab').iter_symbols()}
    for cu in elf.get_dwarf_info().iter_CUs():
        unit = cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
        for die in cu.iter_DIEs():
            if die.tag == 'DW_TAG_enumerator':
                enums[die.attributes['DW_AT_name'].value.decode()] = die.attributes['DW_AT_const_value'].value
            if (unit.endswith('/apps/os/gpu.c') and die.tag == 'DW_TAG_variable' and
                    die.attributes.get('DW_AT_name') and die.attributes['DW_AT_name'].value == b'gpu'):
                image.layouts['gpu_private'] = {d.attributes['DW_AT_name'].value.decode():
                                                d.attributes['DW_AT_data_member_location'].value
                                                for d in die.get_DIE_from_attribute('DW_AT_type').iter_children()
                                                if d.tag == 'DW_TAG_member'}


def read(address, n):
    return next(data[address - start:address - start + n] for start, data in image.parts
                if start <= address and address + n <= start + len(data))


version, stage, limit = struct.unpack('<3I', read(image.address('os_gpu_bisect_configuration'), 12))
assert version == 1 and 1 <= stage <= 4
# The layer table is linked only where the layer limit reads it.
layers = read(image.address('os_placement_layer'), symbols['os_placement_layer']['st_size']) if stage == 4 else b''
spec = importlib.util.spec_from_file_location('gpu_return', ROOT / 'apps/os/gpu-return.py')
decoder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(decoder)
epoch, generation = 123, 456
CLOSE, PARK = 2, enums['PVR_GPU_SERVICE_PARK']


def require(ok, why):
    if not ok:
        raise ValueError(why)


def machine(case):
    u = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    u.mem_map(0x41000000, 0x1200000)
    u.mem_map(0x80000000, 0x10000)
    for address, data in image.parts:
        if data:
            u.mem_write(address, data)
    # Images with breadcrumbs issue SVC #0x736 at each progress point.
    if hasattr(harness, 'skip_crumbs'):
        harness.skip_crumbs(u, image)

    def put(address, value, n=4): u.mem_write(address, int(value).to_bytes(n, 'little'))
    def get(address, n=4): return int.from_bytes(u.mem_read(address, n), 'little')
    def field(base, typ, name): return base + image.layouts[typ][name]
    def sf(base, typ, name, value, n=4): put(field(base, typ, name), value, n)
    m = dict(u=u, put=put, get=get, field=field, sf=sf)
    gpu = image.address('gpu', '/apps/os/gpu.c')
    m.update(gpu=gpu, service=field(gpu, 'gpu_private', 'service'),
             component=field(field(gpu, 'gpu_private', 'session'), 'a733_gpu_mimus', 'component'),
             power=field(gpu, 'gpu_private', 'power'), board=image.address('board'),
             call=image.address('call', '/apps/os/gpu_hook.cc'), record=image.address('os_dispatch_record'),
             completed=image.address('completed', '/apps/os/gpu_hook.cc'),
             jobs=image.address('_ZN12_GLOBAL__N_111bisect_jobsE'),
             reached=image.address('_ZN12_GLOBAL__N_114bisect_reachedE'))
    session = field(m['board'], 'board_application', 'session')
    put(image.address('os_recognizer'), 0x1234, 8)
    sf(m['board'], 'board_application', 'live_until', 60000000, 8)
    sf(session, 'a733_board_session', 'deadline_us', 60000000, 8)
    sf(session, 'a733_board_session', 'epoch', epoch, 8)
    sf(session, 'a733_board_session', 'generation', generation, 8)
    state = dict(time=100, operation=None, ticket=0, suspended=0, hook_removed=0, requests=[], exit=None,
                 begun=0, thermal=0)
    names = ('board_live', 'a733_smp_counter', 'xTaskGetTickCount', 'xTaskGetCurrentTaskHandle',
             'a733_gpu_backend_stopped', 'pvr_gpu_service_send_epoch', 'pvr_gpu_service_collect',
             'uxQueueMessagesWaiting', 'vTaskDelay', 'vTaskSuspend', 'board_fail', 'platform_exit',
             'a733_gpu_mimus_begin', 'a733_gpu_mimus_thermal',
             '_ZN5mimus3asr15SetExactSumHookEPKNS0_12ExactSumHookE')
    hooks = {image.address(n): n for n in names if n in image.symbols}

    def ret(value=0):
        u.reg_write(UC_ARM64_REG_X0, value)
        u.reg_write(UC_ARM64_REG_PC, u.reg_read(UC_ARM64_REG_LR))

    def hook(uc, address, size, unused):
        name = hooks.get(address)
        if name is None:
            return
        args = [uc.reg_read(UC_ARM64_REG_X0 + i) for i in range(5)]
        if name == 'platform_exit': state['exit'] = args[0]; uc.emu_stop()
        elif name in ('board_fail', 'vTaskDelay'): state['time'] += 1000 * (name == 'vTaskDelay'); ret()
        elif name == 'board_live': ret(1)
        elif name == 'a733_smp_counter': ret(state['time'] * 24)
        elif name == 'xTaskGetTickCount': ret(state['time'] // 1000)
        elif name == 'xTaskGetCurrentTaskHandle': ret(0x1234)
        elif name == 'a733_gpu_backend_stopped': ret(1)
        elif name == 'uxQueueMessagesWaiting': ret(0)
        elif name == 'a733_gpu_mimus_begin': state['begun'] += 1; ret(1)
        elif name == 'a733_gpu_mimus_thermal': state['thermal'] += 1; ret(0)
        elif name == 'pvr_gpu_service_send_epoch':
            op = get(field(args[2], 'pvr_gpu_service_request', 'operation'))
            state['operation'] = op; state['ticket'] += 1; state['requests'].append(op)
            put(args[4], state['ticket'], 8); ret(1)
        elif name == 'pvr_gpu_service_collect':
            state['time'] += 1000
            op = state['operation']
            sf(args[2], 'pvr_gpu_service_reply', 'result', 0)
            sf(args[2], 'pvr_gpu_service_reply', 'epoch', 3, 8)
            sf(args[2], 'pvr_gpu_service_reply', 'phase',
               enums['PVR_GPU_SERVICE_PARKED'] if op == PARK else enums['PVR_GPU_SERVICE_STOPPED'])
            ret(1)
        elif name == 'vTaskSuspend':
            require(args[0] == 0x4567, 'suspended another task')
            require(stage == 1 or state['requests'][-1:] == [PARK], 'worker suspended before PARK')
            require(state['hook_removed'] == 1, 'worker suspended before the hook was removed')
            state['suspended'] += 1; ret()
        else:
            require(not args[0], 'hook replaced instead of removed'); state['hook_removed'] += 1; ret()
    u.hook_add(UC_HOOK_CODE, hook)

    def invoke(name, *args):
        u.reg_write(UC_ARM64_REG_SP, 0x8000f000)
        u.reg_write(UC_ARM64_REG_LR, 0x80000000)
        for i, value in enumerate(args):
            u.reg_write(UC_ARM64_REG_X0 + i, value)
        u.emu_start(image.address(name), 0x80000000, count=2000000)
        require(state['exit'] is not None or u.reg_read(UC_ARM64_REG_PC) == 0x80000000, case + ': budget exhausted')
        return u.reg_read(UC_ARM64_REG_X0)
    m.update(state=state, invoke=invoke)
    return m


def opened_session(m, jobs):
    """A session OPEN accepted (stages 2-4), every submitted job completed OK."""
    gpu, sf = m['gpu'], m['sf']
    sf(gpu, 'gpu_private', 'started', 1); sf(gpu, 'gpu_private', 'opened', 1)
    sf(gpu, 'gpu_private', 'epoch', 3, 8); sf(gpu, 'gpu_private', 'component', m['component'], 8)
    sf(m['service'], 'pvr_gpu_service', 'worker', 0x4567, 8)
    sf(m['component'], 'pvr_mimus_service', 'sequence', jobs)
    sf(m['component'], 'pvr_mimus_service', 'slot_state', enums['PVR_MIMUS_SLOT_CLOSED'])
    sf(m['record'], 'os_dispatch_record', 'opened', 1)
    m['put'](m['completed'], jobs); m['put'](m['jobs'], jobs)


def cold_worker(m):
    """Stage 1: constructor complete, worker created and bound, OPEN never sent."""
    sf = m['sf']
    sf(m['gpu'], 'gpu_private', 'started', 1)
    sf(m['service'], 'pvr_gpu_service', 'worker', 0x4567, 8)
    sf(m['service'], 'pvr_gpu_service', 'requests', 0x4000, 8)
    sf(m['service'], 'pvr_gpu_service', 'phase', enums['PVR_GPU_SERVICE_COLD'])


rows = []
jobs = limit if stage == 3 else 0 if stage <= 2 else 7
# Nominal retirement: the version-3 report, accepted by the host decoder.
m = machine('nominal')
(cold_worker if stage == 1 else lambda m: opened_session(m, jobs))(m)
require(m['invoke']('os_gpu_retire') == 1 and m['invoke']('os_gpu_return_ready'), 'nominal retirement refused')
raw = bytes(m['u'].mem_read(image.address('os_gpu_return_report'), 64))
expected = (0x3152475033333741, 3, 64, epoch, generation, jobs, 0 if stage == 1 else 63, stage << 32 | limit)
require(struct.unpack('<8Q', raw) == expected, 'version-3 report differs')
require(m['state']['requests'] == ([] if stage == 1 else [CLOSE, PARK]) and m['state']['suspended'] == 1,
        'retirement requests or worker suspension differ')
decoded = decoder.decode(raw, epoch, generation, require, allow_untouched=True)
require(decoded['jobs'] == jobs and not decoded['qualification_passed'] and
        decoded['gpu_bisect'] == {'stage': decoder.BISECT_STAGES[stage], 'limit': limit}, 'host decode differs')
text = b'MODEL ONLY bisect transcript\n'
m['u'].mem_write(0x80001000, text)
m['state']['time'] = 56000000
m['invoke']('os_cpu_return_supervise')
m['invoke']('os_cpu_return_begin', 0x80001000, len(text))
require(m['state']['exit'] is None, 'CPU completion refused after bisect retirement')
(a.output / 'nominal-gpu-report.bin').write_bytes(raw)
rows.append({'name': 'nominal', 'passed': True, 'report': list(expected), 'decoded': decoded,
             'requests': m['state']['requests']})
try:
    decoder.decode(raw, epoch, generation, require, allow_untouched=False)
    raise AssertionError('version 3 accepted without the image marker')
except ValueError:
    rows.append({'name': 'decoder-needs-marker', 'passed': True})

# Retirement refusals: each leaves no report and no request.
refusals = {'job-in-flight': lambda m: m['sf'](m['call'], 'Call', 'in_flight', 1, 1),
            'unvalidated-job': lambda m: m['put'](m['jobs'], jobs + 1),
            'incomplete-output': lambda m: m['sf'](m['record'], 'os_dispatch_record', 'incomplete', 1),
            'dispatch-failure': lambda m: m['sf'](m['record'], 'os_dispatch_record', 'failure', 2)}
if stage == 1:
    refusals['opened-session'] = lambda m: m['sf'](m['record'], 'os_dispatch_record', 'opened', 1)
else:
    refusals['never-opened'] = lambda m: m['sf'](m['record'], 'os_dispatch_record', 'opened', 0)
for name, mutate in refusals.items():
    m = machine(name)
    (cold_worker if stage == 1 else lambda m: opened_session(m, jobs))(m)
    mutate(m)
    require(m['invoke']('os_gpu_retire') == 0 and not m['invoke']('os_gpu_return_ready'), name + ' accepted')
    require(m['get'](image.address('os_gpu_return_report'), 8) == 0 and not m['state']['requests'] and
            not m['state']['suspended'], name + ' left a report, request or suspension')
    rows.append({'name': name, 'passed': True})

# The limit itself.
if stage == 3:
    for submitted in (limit - 1, limit):
        m = machine('submit-%d' % submitted)
        m['sf'](m['call'], 'Call', 'rows', 64, 8); m['sf'](m['call'], 'Call', 'job_rows', 32)
        m['put'](m['jobs'], submitted)
        result = m['invoke']('_ZN12_GLOBAL__N_16SubmitEv') & 0xff
        under = submitted < limit
        require(result == under and m['state']['begun'] == under and
                m['get'](m['jobs']) == submitted + under and
                m['get'](m['field'](m['call'], 'Call', 'stopped'), 1) == (not under) and
                m['get'](m['reached'], 1) == (not under), 'job limit differs at %d' % submitted)
        rows.append({'name': 'submit-%d-of-%d' % (submitted + 1, limit), 'passed': True, 'submitted': under})
if stage == 4:
    above = next(i for i, v in enumerate(layers) if v > limit)
    within = next(i for i, v in enumerate(layers) if v <= limit)
    for ordinal in (within, above):
        m = machine('layer-%d' % ordinal)
        m['put'](image.address('os_dispatch_active'), 586)
        m['invoke']('_ZN12_GLOBAL__N_15BeginEPvjPKammmmPKsm', 0, ordinal)
        admitted = layers[ordinal] <= limit
        require(m['state']['thermal'] == admitted and m['get'](m['reached'], 1) == (not admitted),
                'layer limit differs at ordinal %d' % ordinal)
        rows.append({'name': 'layer-%d-ordinal-%d' % (layers[ordinal], ordinal), 'passed': True,
                     'admitted': admitted})
save(a.output / 'results.json', {'physical_access': False, 'image_sha256': hashlib.sha256(a.image.read_bytes()).hexdigest(),
                                 'bisect': {'stage': stage, 'limit': limit}, 'cases': rows})
print('PASS linked GPU bisect stage %d limit %d:' % (stage, limit), len(rows), 'cases')
