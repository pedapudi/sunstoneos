#!/usr/bin/env python3
"""Compare general-layout native assembly with the pinned SDK loader.

The pinned AArch64 loader runs in Unicorn with the synthetic HAL of
nbg-loader-reference.py. Every input and output tensor of a session-catalogue
model is bound, and the trace stops at the intercepted submission, before any
device could execute. The native decoder and assembler then build the same
model through nbg_assembly_layout and nbg_assemble_layout at different device
addresses. Expected bytes are the SDK buffers with each relocated address
moved to the native allocation and the completion event changed from 4 to 0.
Allocation minimums must equal the SDK's allocation sizes.
"""
import argparse
import ctypes as c
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import harness  # noqa: E402,F401  (assertion policy)

ROOT = Path(__file__).resolve().parents[1]
MAX_TENSORS = 8


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class Requirement(c.Structure):
    _fields_ = [(n, c.c_uint32) for n in ('payload_bytes', 'minimum_bytes', 'allocation_bytes')]


class Layout(c.Structure):
    _fields_ = [('buffers', Requirement * (4 + 2 * MAX_TENSORS))] + [(n, c.c_uint32) for n in (
        'inputs', 'outputs', 'nn_operations', 'descriptor_patches', 'state_patches',
        'command_body_bytes')]


class Buffer(c.Structure):
    _fields_ = [('cpu', c.c_void_p), ('bytes', c.c_size_t), ('device', c.c_uint32)]


def capture(loader, sdk, model, sections, inputs, outputs, output):
    """SDK preparation of one model with every tensor bound; returns the live reference."""
    ref = loader.Reference(sdk, 0xe0000000)
    ref.call('vip_init', 0)
    address = ref.allocate(len(model))['cpu']
    ref.uc.mem_write(address, model)
    result = ref.allocate(8)['cpu']
    ref.call('vip_create_network', address, len(model), 2, result)
    network = ref.read64(result)
    ref.call('vip_prepare_network', network)
    for section, count, operation in ((1, inputs, 'vip_set_input'), (2, outputs, 'vip_set_output')):
        for index in range(count):
            record = sections[section][0] + index * 112
            assert struct.unpack_from('<I', model, record + 36)[0] == 2
            params = bytearray(48)
            params[:32] = model[record:record + 32]
            struct.pack_into('<I', params, 32, 2)
            params[36:44] = model[record + 40:record + 48]
            pointer = ref.allocate(48)['cpu']
            ref.uc.mem_write(pointer, bytes(params))
            ref.call('vip_create_buffer', pointer, 48, result)
            ref.call(operation, network, index, ref.read64(result))
            ref.bindings.append(dict(operation=operation, index=index,
                                     memory_handle=ref.allocations[-1]['handle']))
    try:
        ref.call('vip_trigger_network', network)
    except loader.SubmissionReached:
        (output / 'trace.json').write_text(json.dumps(dict(
            result='submission_intercepted', network=network, allocations=ref.allocations,
            tasks=ref.tasks, bindings=ref.bindings, calls=ref.calls, physical_access=False,
            sdk_sha256=loader.SDK_SHA, model_sha256=hashlib.sha256(model).hexdigest()), indent=2) + '\n')
        return ref, network
    raise RuntimeError('trigger returned without reaching the intercepted submission')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('sdk', type=Path)
    ap.add_argument('model', type=Path)
    ap.add_argument('native', type=Path, help='host shared library of the decoder and assembler')
    ap.add_argument('output', type=Path, help='new evidence directory')
    args = ap.parse_args()
    loader = module('loader', ROOT / 'checks/nbg-loader-reference.py')
    decoder = module('decoder', ROOT / 'checks/nbg-model.py')
    sdk, data = args.sdk.read_bytes(), args.model.read_bytes()
    assert hashlib.sha256(sdk).hexdigest() == loader.SDK_SHA
    model_sha = hashlib.sha256(data).hexdigest()
    catalogue = json.loads((ROOT / 'tools/npu-session-models.json').read_text())['models']
    assert any(m['model']['sha256'] == model_sha and m['model']['bytes'] == len(data) for m in catalogue), \
        'model is absent from the session catalogue'
    assert struct.unpack_from('<I', data, 4)[0] == 0x20000
    count = struct.unpack_from('<I', data, 56)[0]
    sections = {struct.unpack_from('<I', data, 60 + i * 44)[0]: struct.unpack_from('<QQ', data, 64 + i * 44)
                for i in range(count)}
    args.output.mkdir(parents=True)

    native = decoder.Native(args.native, data)
    lib = native.lib
    lib.nbg_assembly_layout.argtypes = [c.POINTER(decoder.View), c.POINTER(Layout)]
    lib.nbg_assemble_layout.argtypes = [c.POINTER(decoder.View), c.POINTER(Buffer), c.c_size_t,
                                        c.POINTER(Layout)]
    plan, actual = Layout(), Layout()
    assert lib.nbg_assembly_layout(c.byref(native.view), c.byref(plan)) == 1
    slots = 4 + plan.inputs + plan.outputs
    buffers, storage, device = (Buffer * slots)(), [], 0x20000000
    for i in range(slots):
        req = plan.buffers[i]
        if not req.allocation_bytes:
            assert i == 2
            storage.append(None)
            continue
        raw = c.create_string_buffer(req.allocation_bytes + 12288)
        c.memset(c.addressof(raw), 0xa5, c.sizeof(raw))
        cpu = (c.addressof(raw) + 8191) & -4096
        buffers[i] = Buffer(cpu, req.allocation_bytes, device)
        device += req.allocation_bytes + 4096
        storage.append(raw)
    # A slot count other than the plan's is refused before any byte changes.
    assert lib.nbg_assemble_layout(c.byref(native.view), buffers, slots - 1, c.byref(actual)) == 0
    assert all(c.string_at(b.cpu, 64) == b'\xa5' * 64 for b in buffers if b.bytes)
    assert lib.nbg_assemble_layout(c.byref(native.view), buffers, slots, c.byref(actual)) == 1
    assert bytes(plan) == bytes(actual)

    ref, network = capture(loader, sdk, data, sections, plan.inputs, plan.outputs, args.output)
    videos = [v for v in ref.allocations if v['video']]
    by_cpu, by_handle = {v['cpu']: v for v in videos}, {v['handle']: v for v in videos}
    properties, = [t['arguments'] for t in ref.tasks if t['operation'] == 'property']
    binding = {(b['operation'], b['index']): by_handle[b['memory_handle']] for b in ref.bindings}
    pool = ref.read64(network + 0x3a0)
    old = [by_cpu[ref.read64(network + 0x2f8)], by_cpu[ref.read64(network + 0x2d8)],
           by_cpu[pool] if pool else None, by_handle[properties[3]]]
    old += [binding['vip_set_input', i] for i in range(plan.inputs)]
    old += [binding['vip_set_output', i] for i in range(plan.outputs)]
    assert bool(pool) == bool(plan.buffers[2].allocation_bytes)
    for i in range(slots):
        if i != 3:
            assert plan.buffers[i].minimum_bytes == (old[i]['bytes'] if old[i] else 0), f'slot {i} size'
    assert plan.command_body_bytes == properties[4]
    assert plan.buffers[3].payload_bytes == old[3]['bytes']
    read = lambda v: bytes(ref.uc.mem_read(v['cpu'], v['bytes']))
    expected_nn, expected_command = bytearray(read(old[0])), bytearray(read(old[3]))
    coefficients, states = native.table_entries(22), native.table_entries(26)
    records = native.patches()
    command_at, nn_patches, command_patches, routes = 48, 0, 0, []
    for kind, index, _, state_index, first, patch_count, *_ in native.operations():
        if kind == 0xffff:
            continue
        assert kind in (2, 8)  # neural-network or neural-transpose operation
        for patch_kind, destination, source, source_index, source_offset, transformed, *_ in \
                records[first:first + patch_count]:
            if patch_kind == 0:
                at = command_at + destination
                value, = struct.unpack_from('<I', expected_command, at)
                assert (value & ~63) == old[0]['device'] + index * 192
                struct.pack_into('<I', expected_command, at, value + buffers[0].device - old[0]['device'])
                command_patches += 1
                continue
            at = index * 192 + destination
            value, = struct.unpack_from('<I', expected_nn, at)
            if source == 8:
                prior, new = 0x400000, 0x400000
            else:
                slot = {1: 4 + source_index, 2: 4 + plan.inputs + source_index, 3: 2, 5: 1}[source]
                prior, new = old[slot]['device'], buffers[slot].device
                if source == 5:
                    prior += coefficients[source_index][0]
                    new += coefficients[source_index][0]
                if source in (1, 2):
                    routes.append(dict(operation=index, field=destination,
                                       tensor=('input' if source == 1 else 'output'), index=source_index))
            address = (value & 0x03ffffff) << 6 if transformed else value
            assert address == prior + source_offset
            moved = address + new - prior
            assert 0 < moved <= 0xffffffff
            struct.pack_into('<I', expected_nn, at, (value & 0xfc000000) | (moved >> 6) if transformed else moved)
            nn_patches += 1
        command_at += states[state_index][1]
    assert command_at == plan.command_body_bytes
    assert struct.unpack_from('<I', expected_command, command_at + 28) == (0x44,)
    struct.pack_into('<I', expected_command, command_at + 28, 0x40)
    assert (nn_patches, command_patches) == (plan.descriptor_patches, plan.state_patches)
    payloads = [bytes(expected_nn), read(old[1]), read(old[2]) if old[2] else b'', bytes(expected_command)]
    payloads += [read(v) for v in old[4:]]
    for i, (buf, payload) in enumerate(zip(buffers, payloads, strict=True)):
        if not buf.bytes:
            assert payload == b''
            continue
        pad = buf.bytes - len(payload)
        want = payload + (struct.pack('<II', 0x10000000, 0) * (pad // 8) if i == 3 else bytes(pad))
        got = c.string_at(buf.cpu, buf.bytes)
        assert got == want, f'allocation {i} differs from the SDK preparation'
        assert c.string_at(buf.cpu - 4096, 4096) == b'\xa5' * 4096
        assert c.string_at(buf.cpu + buf.bytes, 4096) == b'\xa5' * 4096
    report = dict(result='pass', model_sha256=model_sha, inputs=plan.inputs, outputs=plan.outputs,
                  nn_operations=plan.nn_operations, descriptor_relocations=nn_patches,
                  command_relocations=command_patches, tensor_routes=routes,
                  minimum_bytes=[plan.buffers[i].minimum_bytes for i in range(slots)],
                  sdk_allocation_bytes=[v['bytes'] if v else 0 for v in old],
                  native_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest(),
                  sdk_executed_until='intercepted submission', physical_access=False,
                  submission_authorized=False)
    harness.save(args.output / 'comparison.json', report)
    print(f'PASS layout reference: {plan.inputs} inputs, {plan.outputs} outputs, '
          f'{nn_patches} descriptor and {command_patches} command relocations match the SDK')


if __name__ == '__main__':
    main()
