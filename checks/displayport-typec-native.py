#!/usr/bin/env python3
"""One PD-enabled native Type-C continuation on the composed DisplayPort ELF.

The shared constructor checkpoint supplies the admitted session and all native
provider objects. Native task bodies and driver functions execute unchanged.
Scheduling, architectural services, MMIO, electrical state and the PD sink are
modeled. No board, USB, queue or storage access is performed.
"""
import argparse
import hashlib
import importlib.util
import json
import pickle
import sys
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_LR


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def task_geometry(path):
    result = {}
    def unalias(die):
        while die.tag in ('DW_TAG_typedef', 'DW_TAG_const_type', 'DW_TAG_volatile_type'):
            die = die.get_DIE_from_attribute('DW_AT_type')
        return die
    def array(die):
        dimensions = []
        die = unalias(die)
        while die.tag == 'DW_TAG_array_type':
            for bound in die.iter_children():
                if bound.tag != 'DW_TAG_subrange_type':
                    continue
                if 'DW_AT_count' in bound.attributes:
                    dimensions.append(int(bound.attributes['DW_AT_count'].value))
                else:
                    lower = int(bound.attributes['DW_AT_lower_bound'].value) if 'DW_AT_lower_bound' in bound.attributes else 0
                    dimensions.append(int(bound.attributes['DW_AT_upper_bound'].value)-lower+1)
            die = unalias(die.get_DIE_from_attribute('DW_AT_type'))
        return dimensions, int(die.cu['address_size'] if die.tag == 'DW_TAG_pointer_type' else die.attributes['DW_AT_byte_size'].value)
    with Path(path).open('rb') as stream:
        elf = ELFFile(stream)
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                name = die.attributes.get('DW_AT_name')
                if die.tag != 'DW_TAG_structure_type' or not name or name.value not in (b'dp_application', b'board_application'):
                    continue
                fields = {}
                for member in die.iter_children():
                    key = member.attributes.get('DW_AT_name')
                    if member.tag == 'DW_TAG_member' and key and key.value in (b'tasks', b'stacks', b'handles'):
                        fields[key.value.decode()] = array(member.get_DIE_from_attribute('DW_AT_type'))
                if fields:
                    key = name.value.decode()
                    assert key not in result or result[key] == fields, ('ambiguous task geometry', key)
                    result[key] = fields
    assert set(result) == {'dp_application', 'board_application'}
    return result


def structure_bytes(path, name):
    sizes = set()
    with Path(path).open('rb') as stream:
        elf = ELFFile(stream)
        for cu in elf.get_dwarf_info().iter_CUs():
            for die in cu.iter_DIEs():
                field = die.attributes.get('DW_AT_name')
                if die.tag == 'DW_TAG_structure_type' and field and field.value == name.encode() and 'DW_AT_byte_size' in die.attributes:
                    sizes.add(int(die.attributes['DW_AT_byte_size'].value))
    assert len(sizes) == 1, ('ambiguous structure size', name, sizes)
    return sizes.pop()


def checkpoint_inputs(directory, state):
    records = {}
    for address, count, source in state['ram']:
        if isinstance(source, bytes):
            assert len(source) == count
            continue
        path = Path(source).resolve()
        assert path.parent == directory.resolve() and path.stat().st_size == count
        assert address >= 0x41000000
        records[str(path)] = sha(path)
    return records


def run(args):
    root = args.repository.resolve()
    directory = args.constructor.resolve()
    assert not args.output.exists(), 'preserve earlier execution and checkpoints'
    args.output.mkdir(parents=True)
    raw = (args.fixture/'session.bin').read_bytes()
    contract = args.contract.read_bytes()
    summary = json.loads((directory/'summary.json').read_text())
    proof = summary['allocation']
    assert summary['passed'] and proof['model'] and not proof['physical_authority']
    assert sha(args.image) == proof['image_sha256'] == args.image_sha256
    assert hashlib.sha256(raw).hexdigest() == proof['session_sha256']
    assert len(raw) == 952 and len(contract) == 288
    manifest_path = directory/'constructor-manifest.json'
    assert sha(manifest_path) == args.constructor_manifest_sha256
    manifest = json.loads(manifest_path.read_text())
    assert manifest['schema'] == 'a733-displayport-constructor-checkpoint-v1' and manifest['physical'] is False
    assert manifest['image_sha256'] == args.image_sha256
    assert manifest['session_sha256'] == hashlib.sha256(raw).hexdigest()
    assert manifest['contract_sha256'] == hashlib.sha256(contract).hexdigest()
    assert manifest['machine_sha256'] == sha(root/'checks/displayport-native.py')
    for name, record in manifest['files'].items():
        path = (directory/name).resolve()
        assert path.parent == directory and path.stat().st_size == record['bytes']
        assert sha(path) == record['sha256'], ('mixed constructor checkpoint', name)
    assert {'constructor.pickle', 'summary.json'} <= set(manifest['files'])
    state_path = directory/'constructor.pickle'
    state = pickle.loads(state_path.read_bytes())
    inputs = {str(args.image.resolve()): sha(args.image), str(state_path): sha(state_path),
              str(manifest_path): sha(manifest_path),
              str(args.fixture/'session.bin'): hashlib.sha256(raw).hexdigest(),
              str(args.contract.resolve()): hashlib.sha256(contract).hexdigest(),
              str(directory/'summary.json'): sha(directory/'summary.json')}
    ram_inputs = checkpoint_inputs(directory, state)
    for name, digest in ram_inputs.items():
        assert Path(name).name in manifest['files'] and manifest['files'][Path(name).name]['sha256'] == digest
    inputs.update(ram_inputs)
    for path in [Path(__file__).resolve(), Path(__file__).with_name('displayport-typec-hardware.py'),
                 root/'checks/displayport-native.py', root/'checks/npu-large-native.py',
                 root/'checks/npu-graph-native.py', root/'checks/npu-runtime.py',
                 root/'checks/storage-read-native.py', root/'apps/npu-runtime/host.py',
                 root/'apps/npu-runtime-large/host.py']:
        inputs[str(path)] = sha(path)
    inputs[str(Path(sys.executable).resolve())] = sha(Path(sys.executable).resolve())
    import elftools, unicorn
    for package in (elftools, unicorn):
        for dependency in Path(package.__file__).parent.rglob('*'):
            if dependency.is_file() and (dependency.suffix == '.py' or '.so' in dependency.name):
                inputs[str(dependency.resolve())] = sha(dependency)
    virtualenv = Path(sys.prefix)/'pyvenv.cfg'
    if virtualenv.is_file():
        inputs[str(virtualenv)] = sha(virtualenv)
    snapshots = {}
    for index, (name, digest) in enumerate(inputs.items()):
        source = Path(name)
        if source.stat().st_size > 4*1024*1024:
            continue  # Large constructor RAM remains in its immutable producer checkpoint.
        destination = args.output/'input-snapshots'/f'{index}-{source.name}'
        destination.parent.mkdir(exist_ok=True)
        value = source.read_bytes()
        assert hashlib.sha256(value).hexdigest() == digest
        destination.write_bytes(value)
        snapshots[name] = dict(snapshot=str(destination.relative_to(args.output)), sha256=digest)
    (args.output/'input-snapshots.json').write_text(json.dumps(snapshots, indent=2)+'\n')
    (args.output/'inputs.json').write_text(json.dumps(inputs, indent=2)+'\n')
    base = module('displayport_composed_machine', root/'checks/displayport-native.py')
    hardware = module('displayport_typec_hardware', Path(__file__).with_name('displayport-typec-hardware.py'))

    class Machine(base.Machine):
        def __init__(self, image, session, contract):
            super().__init__(image, session, contract)
            self.inline_yields = set()
            with args.image.open('rb') as stream:
                text = ELFFile(stream).get_section_by_name('.text')
                code = text.data()
                for offset in range(0, len(code)-3, 4):
                    if int.from_bytes(code[offset:offset+4], 'little') == 0xd4000d21:
                        address = int(text['sh_addr'])+offset
                        self.inline_yields.add(address)
                        self.u.hook_add(UC_HOOK_CODE, self.inline_yield, begin=address, end=address)
            if image.symbols.get('vTaskSuspend'):
                address = image.address('vTaskSuspend')
                self.hooks[address] = 'vTaskSuspend'
                self.u.hook_add(UC_HOOK_CODE, self.code, begin=address, end=address)

        def inline_yield(self, cpu, at, size, unused):
            assert self.get(at, 4) == 0xd4000d21
            self.trace.append('inline-port-yield-svc105')
            self.boundary = ('yield', 0)
            self.u.reg_write(UC_ARM64_REG_PC, at+4)
            self.stopped = True
            self.u.emu_stop()

        def code(self, cpu, at, size, unused):
            name = self.hooks.get(at)
            if name in ('vTaskDelay', 'vPortYield', 'vTaskSuspend'):
                if name == 'vTaskSuspend':
                    assert self.u.reg_read(UC_ARM64_REG_X0) in (0, self.task), 'fixture supports self-suspend only'
                ticks = self.u.reg_read(UC_ARM64_REG_X0) if name == 'vTaskDelay' else 0
                self.boundary = ('delay', ticks) if name == 'vTaskDelay' else ('suspend', 0) if name == 'vTaskSuspend' else ('yield', 0)
                self.trace.append(name)
                self.ret()
                self.stopped = True
                self.u.emu_stop()
                return
            if name == 'a733_counter_clock_now':
                # A deterministic logical clock lets modeled wire operations
                # complete while an active task yields. It is no measurement.
                self.time += 1
            super().code(cpu, at, size, unused)

    image = base.base.Image(args.image)
    m = Machine(image, raw, contract)
    for address, count, source in state['ram']:
        if address < 0x41000000:
            m.map_mmio(address, count)
        m.u.mem_write(address, source if isinstance(source, bytes) else Path(source).read_bytes())
    m.u.context_restore(state['cpu'])
    for key in ('time', 'scheduler', 'task', 'tasks', 'holders', 'registers', 'clocks', 'mapped_pages'):
        setattr(m, key, state[key])
    # Restoring RAM must not replace the selected executable or immutable
    # admission source with another image's checkpoint.
    with args.image.open('rb') as stream:
        elf = ELFFile(stream)
        for segment in elf.iter_segments():
            if segment['p_type'] == 'PT_LOAD' and not (int(segment['p_flags']) & 2):
                expected = segment.data()
                assert bytes(m.u.mem_read(int(segment['p_vaddr']), len(expected))) == expected
    assert bytes(m.u.mem_read(0x41f00000, len(raw))) == raw
    assert bytes(m.u.mem_read(m.record, len(contract))) == contract
    assert bytes(m.u.mem_read(0x42004000, len(contract))) == contract
    # Actual constructor RAM is already admitted and retained. No provider
    # object, token, task handle or session field is manufactured here.
    model = hardware.TypeCHardware(lambda: m.time, polarity=1)
    ranges = [(0x03400000, 0x1000), (0x07010000, 0x1000),
              (0x07025000, 0x1000), (0x07083000, 0x2000), (0x07090000, 0x1000)]
    for address, count in ranges:
        m.map_mmio(address, count)
    def read(address, width):
        value = model.read(address)
        return m.registers.get(address, 0) if value is None else value
    def write(address, width, value):
        nonlocal overlap
        model.write(address, value)
        # Evidence requires an actual controller operation during simultaneous
        # native ownership, not an old outstanding wire state.
        if (address == model.TWI1+0x0c and value in (0x48, 0x4c, 0x58, 0x68)
                and m.holders.get(mutex0) == pmic_handle and m.holders.get(mutex1) == handles[1]
                and m.get(native+df['stage'], 4) == 3):
            request = native+df['requests']
            request_id = m.get(request+request_fields['id'])
            expected_owner = m.get(app+m.af['session']+session_fields['owner'])
            assert request_id and m.get(request+request_fields['owner']) == expected_owner
            assert m.get(request+request_fields['lifetime']+lifetime_fields['provider']) == pmic
            active = m.get(pmic+pmic_fields['active'], 4)
            assert active < 8
            active_slot = pmic+pmic_fields['slots']+active*slot_bytes
            active_request = active_slot+slot_fields['request']
            assert m.get(active_request+request_fields['id']) == request_id
            assert m.get(active_request+request_fields['owner']) == expected_owner
            assert m.get(pmic+pmic_fields['held'], 4) == 1
            assert m.get(typec+nf['held'], 4) == 3
            overlap = dict(time_us=m.time, pmic_mutex=mutex0, typec_mutex=mutex1,
                           pmic_holder=pmic_handle, typec_holder=handles[1],
                           pmic_request_id=request_id, pmic_request_owner=expected_owner,
                           display_native_stage=3, progress_address=address, progress_value=value,
                           pmic_wire=model.bus[model.TWI0]['wire'], typec_wire=model.bus[model.TWI1]['wire'])
    m.mmio_read = read
    m.mmio_write = write
    # Initial electrical/controller state is explicitly modeled. Session source
    # capacity, latch authority and recovery remain modeled admission facts.
    initial = {0x07090160: 0x10008002, 0x0701019c: 0x10001,
               0x07025000: 0, 0x07025014: 0, 0x07025024: 0}
    for bus, control in [(model.TWI0, 0x40), (model.TWI1, 0)]:
        for offset, value in [(0x0c, control), (0x10, 0xf8),
                              (0x14, 0x29 if bus == model.TWI0 else 0x28),
                              (0x20, 0x30), (0x200, 0xf81000),
                              (0x210, 0x80c0), (0x218, 0x100010)]:
            initial[bus+offset] = value
    m.registers.update(initial)
    for address, value in initial.items():
        m.put(address, value, 4)
    shared = m.shared
    sf = image.layouts['a733_native_usb']
    nf = image.layouts['a733_typec_native']
    cf = image.layouts['a733_typec']
    pf = image.layouts['a733_typec_pd']
    tf = image.layouts['a733_typec_dp_token']
    app = m.app
    native = app + m.af['native']
    df = image.layouts['dp_native']
    typec = app + m.af['typec']
    cc = typec + nf['provider']
    token = native + df['token']
    handles = [m.get(app+m.af['handles']+i*8) for i in range(3)]
    assert all(handles) and len(set(handles)) == 3
    pmic_handle = m.get(shared+sf['pmic_task'])
    assert pmic_handle and pmic_handle not in handles
    selected = {pmic_handle: 'pmic', handles[0]: 'display', handles[1]: 'typec', handles[2]: 'power'}
    geometry = task_geometry(args.image)
    specifications = {}
    for role, handle, object_at, object_fields, type_name, index, unit, function in [
        ('pmic', pmic_handle, m.board, m.bf, 'board_application', 0, '/apps/board/main.c', 'pmic'),
        ('display', handles[0], app, m.af, 'dp_application', 0, '/apps/displayport/main.c', 'display_task'),
        ('typec', handles[1], app, m.af, 'dp_application', 1, '/apps/displayport/main.c', 'typec_task'),
        ('power', handles[2], app, m.af, 'dp_application', 2, '/apps/displayport/main.c', 'power_task')]:
        item = geometry[type_name]
        count, tcb_bytes = item['tasks']
        stack_dimensions, element = item['stacks']
        assert len(count) == 1 and len(stack_dimensions) == 2 and count[0] == stack_dimensions[0]
        assert element == 8 and item['handles'] == ([count[0]], 8)
        assert index < count[0]
        expected_handle = object_at+object_fields['tasks']+index*tcb_bytes
        assert handle == expected_handle
        assert m.get(object_at+object_fields['handles']+index*8) == handle
        specifications[handle] = dict(role=role, entry=image.address(function, unit=unit),
            depth=stack_dimensions[1], stack=object_at+object_fields['stacks']+index*stack_dimensions[1]*element,
            element=element)

    tasks = []
    for record in m.tasks:
        if record[6] not in selected:
            continue
        entry, name, depth, argument, priority, stack, handle = record
        spec = specifications[handle]
        assert entry == spec['entry'] and depth == spec['depth'] and stack == spec['stack']
        element = spec['element']
        assert priority == (5 if spec['role'] in ('typec', 'power') else 4)
        assert argument == 0 and 0x41000000 <= stack < stack+depth*element <= 0x44000000
        assert not (stack < 0x79000000 and stack+depth*element > 0x48000000)
        tasks.append(dict(role=selected[handle], entry=entry, handle=handle,
                          priority=priority, stack=stack, stack_bytes=depth*element,
                          context=None, wake=m.time, runs=0, yields=0, delays=0, suspends=0))
    for i, first in enumerate(tasks):
        for second in tasks[i+1:]:
            assert first['stack']+first['stack_bytes'] <= second['stack'] or second['stack']+second['stack_bytes'] <= first['stack']
    assert len(tasks) == 4 and {t['role'] for t in tasks} == {'pmic', 'display', 'typec', 'power'}
    assert m.get(typec+nf['config']+image.layouts['a733_typec_native_config']['provider_task']) == handles[1]
    assert m.get(typec+nf['config']+image.layouts['a733_typec_native_config']['power_task']) == handles[2]
    electrical = cc+cf['config']
    ef = image.layouts['a733_typec_config']
    session_fields = image.layouts['a733_displayport_session']
    assert m.get(typec+nf['self']) == typec and m.get(cc+cf['self']) == cc
    assert m.get(electrical+ef['displayport'], 4) == 1
    assert m.get(electrical+ef['source_min_mv'], 4) == 4750
    assert m.get(electrical+ef['source_max_mv'], 4) == 5250
    assert m.get(electrical+ef['source_ma'], 4) == 500
    assert m.get(electrical+ef['zero_threshold_max_mv'], 4) == 800
    assert m.get(electrical+ef['device_id'], 1) == 0x80
    assert m.get(electrical+ef['owner']) == m.get(app+m.af['session']+session_fields['typec_owner'])
    assert m.get(electrical+ef['qualification']) == m.get(app+m.af['session']+session_fields['typec_qualification'])
    assert m.get(electrical+ef['recovery']) == m.get(app+m.af['session']+session_fields['typec_recovery'])
    assert m.get(app+m.af['session']+session_fields['oscillator'], 4) == initial[0x07090160]
    assert m.get(cc+cf['state'], 4) == 0 and not m.get(cc+cf['revoked'], 4)
    assert not m.get(token+tf['provider'])
    pmic = shared+sf['pmic']
    pmic_fields = image.layouts['a733_pmic']
    pin_fields = image.layouts['a733_pin_config']
    pmic_config_fields = image.layouts['a733_pmic_config']
    pin_size = structure_bytes(args.image, 'a733_pin_config')
    assert pin_size == 3
    def pin_image(pin, config):
        function = m.get(config+pin_fields['function'], 1)
        drive = m.get(config+pin_fields['drive'], 1)
        pull = m.get(config+pin_fields['pull'], 1)
        assert drive <= 3 and pull <= 2
        for address, shift, mask, value in [
            (0x07025000+(pin//8)*4, (pin%8)*4, 15, function),
            (0x07025014+(pin//8)*4, (pin%8)*4, 15, drive),
            (0x07025024+(pin//16)*4, (pin%16)*2, 3, pull)]:
            m.registers[address] = (m.registers.get(address, 0) & ~(mask << shift)) | (value << shift)
            m.put(address, m.registers[address], 4)
        return function
    for index in range(2):
        assert pin_image(index, pmic+pmic_fields['config']+pmic_config_fields['pins']+index*pin_size) == 2
    tc_config = typec+nf['config']
    expected = image.layouts['a733_typec_native_config']['expected']
    for index, pin in enumerate((8, 9, 12, 13)):
        assert pin_image(pin, tc_config+expected+index*pin_size) in (0, 15)
    request_fields = image.layouts['a733_pmic_request']
    lifetime_fields = image.layouts['a733_pmic_lifetime']
    slot_fields = image.layouts['a733_pmic_slot']
    session_fields = image.layouts['a733_displayport_session']
    slot_bytes = structure_bytes(args.image, 'a733_pmic_slot')
    # Enumerate the selected task bodies before running any one of them. Every
    # direct sleep/suspend call and inline port yield must have a model boundary.
    coverage = []
    with args.image.open('rb') as stream:
        elf = ELFFile(stream)
        text = elf.get_section_by_name('.text')
        text_at, code = int(text['sh_addr']), text.data()
        symbols = list(elf.get_section_by_name('.symtab').iter_symbols())
        boundaries = {image.address(name): name for name in ('vTaskDelay', 'vTaskSuspend', 'vPortYield') if image.symbols.get(name)}
        for task in tasks:
            sizes = {int(symbol['st_size']) for symbol in symbols if int(symbol['st_value']) == task['entry'] and symbol['st_info']['type'] == 'STT_FUNC' and int(symbol['st_size'])}
            assert len(sizes) == 1
            sites = []
            for address in range(task['entry'], task['entry']+sizes.pop(), 4):
                instruction = int.from_bytes(code[address-text_at:address-text_at+4], 'little')
                if instruction & 0xffe0001f == 0xd4000001:
                    assert instruction == 0xd4000d21 and address in m.inline_yields
                    sites.append(dict(address=address, kind='inline-port-yield-svc105', modeled=True))
                if instruction & 0xfc000000 == 0x94000000:
                    delta = instruction & 0x03ffffff
                    if delta & 0x02000000:
                        delta -= 0x04000000
                    target = address+delta*4
                    if target in boundaries:
                        assert m.hooks.get(target) == boundaries[target]
                        sites.append(dict(address=address, kind=boundaries[target], target=target, modeled=True))
            assert sites
            coverage.append(dict(role=task['role'], entry=task['entry'], sites=sites))
    (args.output/'scheduling-sites.json').write_text(json.dumps(dict(image_sha256=sha(args.image), tasks=coverage), indent=2)+'\n')
    m.scheduler = 2
    sequence = 0
    phase = 'scheduler-idle'
    overlap = None
    lock_fields = image.layouts['a733_platform_locks']
    locks = shared+sf['locks']
    mutex0 = m.get(locks+lock_fields['mutex']+8)
    mutex1 = m.get(locks+lock_fields['mutex']+16)
    assert mutex0 and mutex1 and mutex0 != mutex1
    last = {t['handle']: -1 for t in tasks}
    try:
        while m.time < 1000000 and sequence < 250000:
            ready = [t for t in tasks if t['wake'] <= m.time]
            if not ready:
                m.time = min(t['wake'] for t in tasks)
                continue
            task = min(ready, key=lambda t: (-t['priority'], last[t['handle']]))
            assert m.get(native+df['stage'], 4) <= 3 or overlap, 'first supply request completed before overlap evidence'
            assert m.get(native+df['stage'], 4) < 6, 'stop before DP controller preparation'
            if task['context'] is None:
                m.u.reg_write(UC_ARM64_REG_SP, (task['stack']+task['stack_bytes']) & ~15)
                m.u.reg_write(UC_ARM64_REG_LR, 0x43fff000)
                m.u.reg_write(UC_ARM64_REG_X0, 0)
                pc = task['entry']
            else:
                m.u.context_restore(task['context'])
                pc = m.u.reg_read(UC_ARM64_REG_PC)
            m.task = task['handle']
            m.boundary = None
            m.callname = task['role']
            m.stopped = False
            phase = 'scheduler-task'
            m.u.emu_start(pc, 0, count=10000000)
            assert m.stopped and m.boundary, (task['role'], hex(m.u.reg_read(UC_ARM64_REG_PC)), m.trace[-20:])
            task['context'] = m.u.context_save()
            phase = 'scheduler-boundary'
            task['runs'] += 1
            kind, ticks = m.boundary
            if kind == 'yield':
                task['yields'] += 1
                task['wake'] = m.time
                m.time += 1
            elif kind == 'suspend':
                task['suspends'] += 1
                task['wake'] = (1 << 64)-1
            else:
                assert ticks == 1
                task['delays'] += 1
                task['wake'] = (m.time//1000+ticks)*1000
            last[task['handle']] = sequence
            sequence += 1
            assert not m.get(cc+cf['revoked'], 4), ('Type-C revoked', m.time, m.get(cc+cf['state'], 4), task['role'])
            assert not m.get(typec+nf['power_fault'], 4)
            if m.get(token+tf['provider']) and m.get(token+tf['hpd'], 4) and overlap:
                break
        else:
            raise AssertionError(('bounded nominal setup did not negotiate', m.time, sequence))
        assert m.get(native+df['stage'], 4) == 3
        assert m.get(native+df['controller']+image.layouts['a733_dp']['self']) == 0
        phase = 'detached-validation'
        m.task = handles[0]
        deadline = m.get(app+m.af['deadline'])
        assert m.call('a733_typec_dp_validate', cc, token, deadline) == 1
        assert m.get(token+tf['provider']) == cc
        assert m.get(token+tf['polarity'], 4) == 1 and m.get(token+tf['pin_assignment'], 4) == 4
        assert m.get(cc+cf['pd']+pf['ready'], 4) == 1 and model.configure_ack
        assert m.get(typec+nf['input_hz'], 4) == 26000000
        assert m.get(typec+nf['divider'], 4) == 97 and m.get(typec+nf['rate_hz'], 4) == 100000
        assert len(model.enable_times) == 1 and model.enable_times[0] >= 120000
        assert model.ack_reads and model.nack_reads and model.completed_stops
        phase = 'detached-validation-complete'
        m.checkpoint(args.output, 'negotiated')
        (args.output/'scheduler.pickle').write_bytes(pickle.dumps(dict(sequence=sequence, last=last, phase=phase, active_handle=None, helper_task_handle=m.task, tasks=tasks)))
        (args.output/'hardware.pickle').write_bytes(pickle.dumps(model.export()))
        (args.output/'tasks.pickle').write_bytes(pickle.dumps(tasks))
        report = dict(passed=True, physical=False, image_sha256=sha(args.image),
                      session_sha256=hashlib.sha256(raw).hexdigest(),
                      contract_sha256=hashlib.sha256(contract).hexdigest(),
                      time_us=m.time, provider=cc, token=token, typec=typec,
                      handles=handles, pmic_handle=pmic_handle,
                      independent_bus_overlap=overlap, checkpoint_phase=phase,
                      qualified_dp_apb_hz=m.get(app+m.af['session']+session_fields['apb_hz'], 4),
                      apb_evidence='session-qualified modeled fact, not a measurement',
                      raw_dcxo_hz=26000000, twi1_divider=97, twi1_hz=100000,
                      transmissions=model.transmissions, configure_ack=model.configure_ack,
                      task_summary=[{k:v for k,v in t.items() if k!='context'} for t in tasks],
                      scope=__doc__)
        for path, digest in inputs.items():
            assert sha(path) == digest, ('input changed during execution', path)
        (args.output/'summary.json').write_text(json.dumps(report, indent=2)+'\n')
        files = {str(path.relative_to(args.output)): dict(sha256=sha(path), bytes=path.stat().st_size)
                 for path in sorted(args.output.rglob('*')) if path.is_file()}
        manifest = dict(schema='a733-displayport-typec-checkpoint-v1', physical=False,
                        image_sha256=sha(args.image), session_sha256=hashlib.sha256(raw).hexdigest(),
                        contract_sha256=hashlib.sha256(contract).hexdigest(),
                        constructor_manifest_sha256=args.constructor_manifest_sha256,
                        sources={str(Path(__file__).resolve()): sha(Path(__file__)),
                                 str(Path(__file__).with_name('displayport-typec-hardware.py').resolve()):
                                 sha(Path(__file__).with_name('displayport-typec-hardware.py'))}, files=files)
        (args.output/'negotiated-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
        print('PASS composed native TYPEC24 negotiation and actual DP token validation')
    except BaseException as error:
        # Capture the active slice, not its already-obsolete previous yield.
        if phase == 'scheduler-task':
            task['context'] = m.u.context_save()
        scheduler = dict(sequence=sequence, last=last, phase=phase,
                         active_handle=m.task if phase == 'scheduler-task' else None,
                         helper_task_handle=m.task if phase.startswith('detached') else None,
                         boundary=getattr(m, 'boundary', None), tasks=tasks)
        if phase.startswith('detached'):
            scheduler['helper_context'] = m.u.context_save()
        (args.output/'stopped-scheduler.pickle').write_bytes(pickle.dumps(scheduler))
        m.checkpoint(args.output, 'stopped')
        (args.output/'stopped-hardware.pickle').write_bytes(pickle.dumps(model.export()))
        (args.output/'stopped-tasks.pickle').write_bytes(pickle.dumps(tasks))
        (args.output/'failure.json').write_text(json.dumps(dict(error=repr(error), time_us=m.time,
          sequence=sequence, trace=m.trace[-40:], physical=False), indent=2)+'\n')
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('repository', 'image', 'fixture', 'contract', 'constructor', 'output'):
        parser.add_argument('--'+name, type=Path, required=True)
    parser.add_argument('--image-sha256', required=True)
    parser.add_argument('--constructor-manifest-sha256', required=True)
    run(parser.parse_args())


if __name__ == '__main__':
    main()
