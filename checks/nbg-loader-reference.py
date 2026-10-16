#!/usr/bin/env python3
"""Trace the pinned NBG loader with a synthetic, host-only HAL in Unicorn."""
import argparse
from collections import Counter
import ctypes
import hashlib
import io
import json
from pathlib import Path
import struct

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import UC_ARM64_REG_X0, UC_ARM64_REG_X30
from unicorn.arm64_const import UC_ARM64_REG_SP, UC_ARM64_REG_PC

SDK_SHA = "82f049b0ed0065dd4d443e37eeb1edfcbef24457c9c24e36170d64d5b748ca66"
MODEL_SHA = "fc0d50bdc863e4dfc74eef6dad9d34655c560e759a2f426979a01c505fa9b95f"
BASE, IMPORTS, STOP = 0x100000, 0x200000, 0x210000
HEAP, HEAP_BYTES, STACK = 0x10000000, 32 << 20, 0x20000000
# vip_hardware_feature_e values from the pinned A733 driver feature database.
FEATURES = {0: 1, 1: 1, 3: 0, 4: 0, 5: 1, 6: 8, 7: 0, 8: 128,
            9: 1024, 10: 256, 11: 256, 12: 16, 13: 1, 14: 1,
            15: 1, 16: 1, 17: 0, 18: 1}


class SubmissionReached(Exception):
    """End the trace at the boundary before a device could execute commands."""


class Patch(ctypes.Structure):
    _fields_ = [('source_bytes', ctypes.c_uint64), ('referenced_bytes', ctypes.c_uint64),
                ('source_device', ctypes.c_uint32), ('source_offset', ctypes.c_uint32),
                ('destination_offset', ctypes.c_uint32), ('encoding', ctypes.c_int)]


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


class Reference:
    def __init__(self, blob, video_base):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.heap = HEAP
        self.video = video_base
        self.allocations = []
        self.calls = Counter()
        self.logs = []
        self.tasks = []
        self.bindings = []
        self.imports = {}
        self.names = {}
        elf = ELFFile(io.BytesIO(blob))
        segments = [s for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
        end = max(s['p_vaddr'] + s['p_memsz'] for s in segments)
        self.uc.mem_map(BASE, (end + 4095) & ~4095)
        for s in segments:
            self.uc.mem_write(BASE + s['p_vaddr'], s.data())
        self.uc.mem_map(IMPORTS, 0x10000)
        self.uc.mem_map(STOP, 0x1000)
        self.uc.mem_map(HEAP, HEAP_BYTES)
        self.uc.mem_map(STACK, 0x10000)
        symbols = elf.get_section_by_name('.dynsym')
        self.exports = {s.name: BASE + s['st_value'] for s in symbols.iter_symbols()
                        if s['st_shndx'] != 'SHN_UNDEF'}
        for section in elf.iter_sections():
            if section['sh_type'] != 'SHT_RELA':
                continue
            for r in section.iter_relocations():
                kind = r['r_info_type']
                if kind == 1027:
                    value = BASE + r['r_addend']
                elif kind in (1025, 1026):
                    sym = symbols.get_symbol(r['r_info_sym'])
                    if sym.name in self.exports:
                        value = self.exports[sym.name] + r['r_addend']
                    else:
                        if sym.name not in self.names:
                            address = IMPORTS + len(self.names) * 4
                            self.names[sym.name] = address
                            self.imports[address] = sym.name
                            self.uc.mem_write(address, bytes.fromhex('c0035fd6'))
                        value = self.names[sym.name]
                else:
                    raise RuntimeError(f'unsupported ELF relocation {kind}')
                self.write64(BASE + r['r_offset'], value)
        self.uc.hook_add(UC_HOOK_CODE, self.import_call, begin=IMPORTS, end=IMPORTS + 0xffff)

    def write32(self, address, value):
        self.uc.mem_write(address, struct.pack('<I', value))

    def write64(self, address, value):
        self.uc.mem_write(address, struct.pack('<Q', value))

    def read64(self, address):
        return struct.unpack('<Q', self.uc.mem_read(address, 8))[0]

    def allocate(self, size, alignment=64, video=False):
        if not 0 < size <= HEAP_BYTES or not alignment or alignment & (alignment - 1):
            raise RuntimeError(f'invalid allocation {size} / {alignment}')
        cpu = (self.heap + alignment - 1) & -alignment
        self.heap = cpu + size + 64
        if self.heap > HEAP + HEAP_BYTES:
            raise RuntimeError('synthetic heap exhausted')
        allocation = {'cpu': cpu, 'bytes': size, 'alignment': alignment,
                      'caller': self.uc.reg_read(UC_ARM64_REG_X30) - BASE,
                      'video': video}
        if video:
            device = (self.video + alignment - 1) & -alignment
            self.video = device + size + 4096
            require(self.video <= 1 << 32, 'synthetic device address space exhausted')
            allocation['device'] = device
        self.allocations.append(allocation)
        return allocation

    def string(self, address):
        result = bytearray()
        for i in range(4096):
            byte = self.uc.mem_read(address + i, 1)[0]
            if byte == 0:
                return result.decode('utf-8', errors='replace')
            result.append(byte)
        raise RuntimeError('unterminated reference string')

    def import_call(self, uc, address, size, user):
        name = self.imports[address]
        a = [uc.reg_read(UC_ARM64_REG_X0 + i) for i in range(8)]
        self.calls[name] += 1
        ret = 0
        if name == 'viphal_allocate_memory':
            self.write64(a[1], self.allocate(a[0])['cpu'])
        elif name == 'calloc':
            ret = self.allocate(a[0] * a[1])['cpu']
        elif name == 'viphal_allocate_videomemory':
            v = self.allocate(a[3], a[4], True)
            v['flags'], v['visibility'] = a[5], a[6]
            v['handle'] = len(self.allocations)
            self.write64(a[0], v['cpu'])
            self.write32(a[1], len(self.allocations))
            self.write64(a[2], v['device'])
        elif name in ('viphal_zero_memory', 'viphal_os_zero_memory', 'memset'):
            n, fill = (a[2], a[1] & 255) if name == 'memset' else (a[1], 0)
            if n > HEAP_BYTES:
                raise RuntimeError('excessive memory fill')
            uc.mem_write(a[0], bytes([fill]) * n)
            ret = a[0] if name == 'memset' else 0
        elif name in ('viphal_copy_memory', 'memcpy', 'memmove'):
            if a[2] > HEAP_BYTES:
                raise RuntimeError('excessive memory copy')
            uc.mem_write(a[0], bytes(uc.mem_read(a[1], a[2])))
            ret = a[0] if name in ('memcpy', 'memmove') else 0
        elif name == 'viphal_os_create_mutex':
            self.write64(a[0], self.allocate(64)['cpu'])
        elif name == 'viphal_query_database':
            db = bytearray(0x188)
            for offset, value in {0: 0x400000, 4: 0x80000, 0x24: 0x480000,
                                  0x4c: 0x1000003b, 0x50: 1, 0x54: 1,
                                  0x58: 1, 0x5c: 1, 0x7c: 0}.items():
                struct.pack_into('<I', db, offset, value)
            uc.mem_write(a[0], bytes(db))
        elif name == 'viphal_query_hw_feature':
            if a[0] not in FEATURES:
                raise RuntimeError(f'unmodeled hardware feature {a[0]}')
            ret = FEATURES[a[0]]
        elif name == 'viphal_task_create':
            self.write32(a[2], 1)
            self.tasks.append({'operation': 'create', 'name': self.string(a[0]), 'priority': a[1]})
        elif name == 'viphal_task_set_property':
            self.tasks.append({'operation': 'property', 'arguments': a[:7]})
        elif name == 'viphal_submit_task':
            self.tasks.append({'operation': 'submission_intercepted', 'arguments': a})
            raise SubmissionReached()
        elif name == 'viphal_os_print':
            self.logs.append({'format': self.string(a[1]), 'arguments': a[2:]})
        elif name == 'viphal_print_string':
            self.logs.append({'string': self.string(a[0])})
        elif name == 'viphal_os_strcmp':
            first, second = self.string(a[0]), self.string(a[1])
            ret = (first > second) - (first < second)
        elif name == 'viphal_os_strcpy':
            uc.mem_write(a[0], self.string(a[1]).encode() + b'\0')
        elif name in ('viphal_os_get_tid', 'viphal_os_get_pid'):
            ret = 1
        elif name in ('viphal_init', 'viphal_destroy', 'viphal_free_memory', 'free',
                      'pthread_mutex_lock', 'pthread_mutex_unlock',
                      'viphal_os_lock_mutex', 'viphal_os_unlock_mutex',
                      'viphal_os_destroy_mutex', 'viphal_free_videomemory',
                      'viphal_flush_cache'):
            pass
        else:
            raise RuntimeError(f'unmodeled import {name}: {a}; caller={uc.reg_read(UC_ARM64_REG_X30)-BASE:#x}')
        uc.reg_write(UC_ARM64_REG_X0, ret & ((1 << 64) - 1))

    def call(self, name, *args):
        for i in range(8):
            self.uc.reg_write(UC_ARM64_REG_X0 + i, args[i] if i < len(args) else 0)
        self.uc.reg_write(UC_ARM64_REG_SP, STACK + 0xfff0)
        self.uc.reg_write(UC_ARM64_REG_X30, STOP)
        self.uc.emu_start(self.exports[name], STOP, timeout=5_000_000, count=5_000_000)
        if self.uc.reg_read(UC_ARM64_REG_PC) != STOP:
            raise RuntimeError(f'{name} exceeded execution budget')
        status = self.uc.reg_read(UC_ARM64_REG_X0) & 0xffffffff
        if status:
            raise RuntimeError(f'{name} returned {status:#x}')

    def snapshot(self, output, label, network):
        directory = output / label
        directory.mkdir()
        (directory/'network.bin').write_bytes(self.uc.mem_read(network, 0x610))
        for allocation in self.allocations:
            if allocation['video']:
                (directory/f"buffer-{allocation['handle']}.bin").write_bytes(
                    self.uc.mem_read(allocation['cpu'], allocation['bytes']))


def reconstruct(ref, model, native_path, output, network, decoded=None, capture=None):
    """Resolve the pinned NN profile independently and compare complete buffers.

    This is a format experiment, not admission of arbitrary model commands.
    Descriptor access extents remain unresolved; most NN relocations therefore
    use a one-byte address check, explicitly insufficient for submission.
    """
    native = ctypes.CDLL(str(native_path.resolve()))
    native.nbg_relocate.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                    ctypes.POINTER(Patch), ctypes.c_size_t]
    native.nbg_relocate.restype = ctypes.c_int

    def section(index):
        if decoded is not None:
            return decoded.section(index)
        offset, size = struct.unpack_from('<II', model, 376 + index * 8)
        return model[offset:offset + size]

    def encode(data, patches):
        target = ctypes.create_string_buffer(bytes(data), len(data))
        array = (Patch * len(patches))(*patches)
        require(native.nbg_relocate(target, len(data), array, len(patches)) == 1,
                'native encoder refused resolved references')
        return target.raw

    videos = [v for v in ref.allocations if v['video']]
    require(len(videos) == 6, 'unexpected SDK allocation count')
    by_cpu = {v['cpu']: v for v in videos}
    by_handle = {v['handle']: v for v in videos}
    nn = by_cpu[ref.read64(network + 0x2f8)]
    coeff = by_cpu[ref.read64(network + 0x2d8)]
    pool = by_cpu[ref.read64(network + 0x3a0)]
    properties = [t['arguments'] for t in ref.tasks if t['operation'] == 'property']
    require(len(properties) == 1, 'unexpected task properties')
    command = by_handle[properties[0][3]]
    require(properties[0][:3] == [1, 0, 0] and properties[0][5:] == [0, 0],
            'unexpected task configuration')
    binding = {(b['operation'], b['index']): by_handle[b['memory_handle']] for b in ref.bindings}
    input_buffer = binding['vip_set_input', 0]
    output_buffer = binding['vip_set_output', 0]
    sram_bytes = decoded.view.header.vip_sram_bytes if decoded is not None else struct.unpack_from('<I', model, 372)[0]
    sources = {1: input_buffer, 2: output_buffer, 3: pool,
               8: {'device': 0x400000, 'bytes': sram_bytes}}
    nn_bytes = section(6)
    weights = section(23)
    coefficients = decoded.table_entries(22) if decoded is not None else list(struct.iter_unpack('<II', section(22)))
    records = decoded.patches() if decoded is not None else list(struct.iter_unpack('<10I', section(9)))
    state_table = decoded.table_entries(26) if decoded is not None else list(struct.iter_unpack('<II', section(26)))
    load_states = section(20)
    operations = decoded.operations() if decoded is not None else list(struct.iter_unpack('<8I', section(3)))
    nn_patches, state_patches, deferred = [], [], []
    states = bytearray(struct.pack('<12I',
        0x08010e02, 0x701, 0x48000000, 0x701, 0x0801022c, 0x1f,
        0x08010e03, 0xc23, 0x08010e02, 0x701, 0x48000000, 0x701))
    source_counts = Counter()
    ignored = []
    nn_count = 0
    for operation, index, layer, state_index, first, count, _, _ in operations:
        if operation == 0xffff:
            ignored.extend(range(first, first + count))
            continue
        require(operation == 2, 'profile contains an unsupported operation')
        nn_count += 1
        start = len(states)
        offset, size = state_table[state_index]
        states.extend(load_states[offset:offset + size])
        for record in records[first:first + count]:
            kind, dest, source, source_index, source_offset, transformed, name, high, mask, dest_base = record
            require(not (name or high or mask or dest_base), 'unsupported patch fields')
            require(transformed in (0, 1), 'unsupported wire transformation')
            if kind == 0:
                require(source == 0 and source_index == 0 and source_offset == 0,
                        'unsupported state reference')
                require(dest + 4 <= size, 'state destination exceeds operation')
                state_patches.append(Patch(nn['bytes'], 192, nn['device'], index * 192,
                                           start + dest, 2 if transformed else 0))
            else:
                require(kind == 1 and dest + 4 <= 192, 'unsupported descriptor destination')
                if source == 5:
                    offset, size = coefficients[source_index]
                    require(offset + size <= len(weights), 'coefficient reference exceeds section')
                    allocation = {'device': coeff['device'] + offset, 'bytes': size}
                else:
                    require(source in sources, 'unsupported descriptor source')
                    allocation = sources[source]
                patch = Patch(allocation['bytes'], 1, allocation['device'], source_offset,
                              index * 192 + dest, transformed)
                nn_patches.append(patch)
                if source in (1, 2):
                    deferred.append(patch.destination_offset)
            source_counts[source] += 1
    require(len(nn_bytes) == nn_count * 192, 'unexpected NN descriptor count')
    expected_nn = encode(nn_bytes, nn_patches)
    expected_states = encode(states, state_patches)
    require(properties[0][4] == len(expected_states), 'submitted body size mismatch')
    footer = struct.pack('<14I', 0x08010e02, 0x30000701, 0x48000000, 0x30000701,
                         0x08010e03, 0xc23, 0x08010e01, 0x44, 0x10000000, 0, 0, 0, 0, 0)
    expected_command = expected_states + footer
    require(expected_nn == bytes(ref.uc.mem_read(nn['cpu'], nn['bytes'])),
            'SDK descriptor contents differ from native reconstruction')
    require(expected_command == bytes(ref.uc.mem_read(command['cpu'], command['bytes'])),
            'SDK command stream differs from native reconstruction')
    require(weights == bytes(ref.uc.mem_read(coeff['cpu'], len(weights))),
            'SDK coefficient payload differs from model section')
    capture = output if capture is None else capture
    bound_nn = (capture/'bound'/f"buffer-{nn['handle']}.bin").read_bytes()
    prepared_nn = (capture/'prepared'/f"buffer-{nn['handle']}.bin").read_bytes()
    require(bound_nn == prepared_nn, 'binding unexpectedly changed descriptors immediately')
    expected_bound = bytearray(expected_nn)
    for offset in deferred:
        expected_bound[offset:offset + 4] = nn_bytes[offset:offset + 4]
    require(bytes(expected_bound) == bound_nn, 'deferred tensor patch locations differ')
    require(ignored == [0], 'unexpected initialization patch inventory')
    (output/'reconstructed-nn.bin').write_bytes(expected_nn)
    (output/'reconstructed-command.bin').write_bytes(expected_command)
    return {'result': 'pass', 'nn_operations': nn_count,
            'descriptor_relocations': len(nn_patches), 'state_relocations': len(state_patches),
            'source_counts': source_counts, 'omitted_init_patch_indices': ignored,
            'deferred_tensor_word_offsets': deferred,
            'coefficient_payload_bytes': len(weights), 'coefficient_allocation_bytes': coeff['bytes'],
            'command_body_bytes': len(expected_states), 'command_allocation_bytes': command['bytes'],
            'input_allocation_bytes': input_buffer['bytes'], 'output_allocation_bytes': output_buffer['bytes'],
            'native_sha256': hashlib.sha256(native_path.read_bytes()).hexdigest(),
            'descriptor_access_extents_verified': False, 'command_safety_verified': False}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sdk', type=Path)
    ap.add_argument('model', type=Path)
    ap.add_argument('native', type=Path, help='host shared library built from nbg_relocation.c')
    ap.add_argument('output', type=Path)
    ap.add_argument('--video-base', type=lambda x: int(x, 0), default=0x30000000)
    args = ap.parse_args()
    sdk, model = args.sdk.read_bytes(), args.model.read_bytes()
    if hashlib.sha256(sdk).hexdigest() != SDK_SHA or hashlib.sha256(model).hexdigest() != MODEL_SHA:
        raise SystemExit('reference identity mismatch')
    # Preserve prior evidence if the same destination is supplied again.
    args.output.mkdir(parents=True)
    require(0 < args.video_base <= 0xfe000000 and args.video_base % 4096 == 0,
            'invalid synthetic device base')
    ref = Reference(sdk, args.video_base)
    error = None
    comparison = None
    network = 0
    try:
        ref.call('vip_init', 0)
        buffer = ref.allocate(len(model))['cpu']
        ref.uc.mem_write(buffer, model)
        result = ref.allocate(8)['cpu']
        ref.call('vip_create_network', buffer, len(model), 2, result)
        network = ref.read64(result)
        ref.snapshot(args.output, 'created', network)
        ref.call('vip_prepare_network', network)
        ref.snapshot(args.output, 'prepared', network)
        # The public buffer-creation structure is 48 bytes. Copy the pinned
        # fixture's tensor fields; all other model identities are refused above.
        for section, operation in ((0, 'vip_set_input'), (1, 'vip_set_output')):
            start, length = struct.unpack_from('<II', model, 376 + section * 8)
            for index, tensor in enumerate(range(start, start + length, 116)):
                params = bytearray(48)
                params[:32] = model[tensor:tensor + 32]
                quant = struct.unpack_from('<I', model, tensor + 36)[0]
                struct.pack_into('<I', params, 32, quant)
                if quant == 1:
                    params[36:40] = model[tensor + 40:tensor + 44]
                elif quant == 2:
                    params[36:44] = model[tensor + 44:tensor + 52]
                pointer = ref.allocate(48)['cpu']
                ref.uc.mem_write(pointer, bytes(params))
                ref.call('vip_create_buffer', pointer, 48, result)
                ref.call(operation, network, index, ref.read64(result))
                ref.bindings.append({'operation': operation, 'index': index,
                                     'memory_handle': ref.allocations[-1]['handle']})
        ref.snapshot(args.output, 'bound', network)
        ref.call('vip_trigger_network', network)
        raise RuntimeError('trigger returned without reaching the submission boundary')
    except SubmissionReached:
        ref.snapshot(args.output, 'submission', network)
        try:
            comparison = reconstruct(ref, model, args.native, args.output, network)
        except Exception as exc:
            error = str(exc)
    except Exception as exc:
        error = str(exc)
    report = {'result': 'incomplete' if error else 'submission_intercepted', 'error': error,
              'pc': hex(ref.uc.reg_read(UC_ARM64_REG_PC) - BASE),
              'network': network, 'allocations': ref.allocations,
              'calls': ref.calls, 'logs': ref.logs, 'tasks': ref.tasks, 'bindings': ref.bindings,
              'comparison': comparison, 'physical_access': False,
              'sdk_sha256': SDK_SHA, 'model_sha256': MODEL_SHA, 'video_base': args.video_base}
    (args.output/'trace.json').write_text(json.dumps(report, indent=2)+'\n')
    if error:
        raise SystemExit(error)


if __name__ == '__main__':
    main()
