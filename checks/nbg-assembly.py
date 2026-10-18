#!/usr/bin/env python3
"""Compare native assembly with a saved SDK submission at different addresses.

The SDK capture is hash-verified and reused; SDK instructions are not executed.
Expected bytes start with the captured SDK buffers, then adjust recorded address
words for page-aligned native allocations and change completion event 4 to 0.
"""
import argparse
import ctypes as c
import hashlib
import importlib.util
import json
from pathlib import Path
import struct


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


class Requirement(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in ('payload_bytes', 'minimum_bytes', 'allocation_bytes')]


class Plan(c.Structure):
    _fields_ = [('buffers', Requirement * 6)] + [(name, c.c_uint32) for name in (
        'nn_operations', 'descriptor_patches', 'state_patches', 'command_body_bytes')]


class Buffer(c.Structure):
    _fields_ = [('cpu', c.c_void_p), ('bytes', c.c_size_t), ('device', c.c_uint32)]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('model', type=Path)
    ap.add_argument('native', type=Path)
    ap.add_argument('capture', type=Path)
    ap.add_argument('output', type=Path)
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    decoder = module('decoder', root/'checks/nbg-model.py')
    reference = module('reference', root/'checks/nbg-loader-reference.py')
    data = args.model.read_bytes()
    model_sha = hashlib.sha256(data).hexdigest()
    catalogue=json.loads((root/'tools/npu-models.json').read_text())['models']
    directory = any(model_sha==v['model']['sha256'] and len(data)==v['model']['bytes'] for v in catalogue)
    assert directory or model_sha == reference.MODEL_SHA
    # The convolution capture deliberately has no previous comparison. Verify
    # its hashes and intercepted-submission status, then perform that comparison.
    capture = decoder.Capture(args.capture, reference.SDK_SHA, model_sha,
                              require_comparison=not directory)
    args.output.mkdir(parents=True)  # Refuse replacement of existing evidence.
    native = decoder.Native(args.native, data)
    lib = native.lib
    lib.nbg_assembly_requirements.argtypes = [c.POINTER(decoder.View), c.POINTER(Plan)]
    lib.nbg_assemble.argtypes = [c.POINTER(decoder.View), c.POINTER(Buffer), c.POINTER(Plan)]
    plan, actual_plan = Plan(), Plan()
    assert lib.nbg_assembly_requirements(c.byref(native.view), c.byref(plan)) == 1
    buffers = (Buffer * 6)()
    storage = []
    device = 0xe0000000
    for i, req in enumerate(plan.buffers):
        if req.allocation_bytes == 0:
            assert i == 2 and req.payload_bytes == req.minimum_bytes == 0
            storage.append(None)
            continue
        raw = c.create_string_buffer(req.allocation_bytes + 12288)
        c.memset(c.addressof(raw), 0xa5, c.sizeof(raw))
        cpu = (c.addressof(raw) + 8191) & -4096
        buffers[i] = Buffer(cpu, req.allocation_bytes, device)
        device += req.allocation_bytes + 4096
        storage.append(raw)
    assert lib.nbg_assemble(c.byref(native.view), buffers, c.byref(actual_plan)) == 1
    assert bytes(plan) == bytes(actual_plan)

    videos = [v for v in capture.allocations if v['video']]
    by_cpu, by_handle = {v['cpu']: v for v in videos}, {v['handle']: v for v in videos}
    network = capture.report['network']
    properties, = [t['arguments'] for t in capture.tasks if t['operation'] == 'property']
    binding = {(b['operation'], b['index']): by_handle[b['memory_handle']] for b in capture.bindings}
    old = [by_cpu[capture.read64(network + offset)] for offset in (0x2f8, 0x2d8)]
    pool = capture.read64(network + 0x3a0)
    old.append(by_cpu[pool] if pool else None)
    assert bool(pool) == bool(plan.buffers[2].allocation_bytes)
    old += [by_handle[properties[3]], binding['vip_set_input', 0], binding['vip_set_output', 0]]
    expected_nn = bytearray(capture.mem_read(old[0]['cpu'], old[0]['bytes']))
    expected_command = bytearray(capture.mem_read(old[3]['cpu'], old[3]['bytes']))
    for i in (0, 1, 2, 4, 5):
        assert plan.buffers[i].minimum_bytes == (old[i]['bytes'] if old[i] else 0)
    assert plan.command_body_bytes == properties[4]
    assert plan.buffers[3].payload_bytes == old[3]['bytes']
    records = native.patches()
    coefficients = native.table_entries(22)
    states = native.table_entries(26)
    command_at, nn_patches, command_patches = 48, 0, 0
    for kind, index, _, state_index, first, count, *_ in native.operations():
        if kind == 0xffff:
            continue
        assert kind == 2
        for p in records[first:first + count]:
            patch_kind, destination, source, source_index, source_offset, transformed, *_ = p
            if patch_kind == 0:
                at = command_at + destination
                word, = struct.unpack_from('<I', expected_command, at)
                assert (word & ~63) == old[0]['device'] + index * 192
                struct.pack_into('<I', expected_command, at,
                                 word + buffers[0].device - old[0]['device'])
                command_patches += 1
                continue
            at = index * 192 + destination
            word, = struct.unpack_from('<I', expected_nn, at)
            if source == 8:
                prior_base = new_base = 0x400000
            else:
                alloc = {1: 4, 2: 5, 3: 2, 5: 1}[source]
                prior_base, new_base = old[alloc]['device'], buffers[alloc].device
                if source == 5:
                    prior_base += coefficients[source_index][0]
                    new_base += coefficients[source_index][0]
            prior_address = (word & 0x03ffffff) << 6 if transformed else word
            assert prior_address == prior_base + source_offset
            new_address = prior_address + new_base - prior_base
            assert 0 < new_address <= 0xffffffff
            encoded = (word & 0xfc000000) | (new_address >> 6) if transformed else new_address
            struct.pack_into('<I', expected_nn, at, encoded)
            nn_patches += 1
        command_at += states[state_index][1]
    assert command_at == plan.command_body_bytes
    assert struct.unpack_from('<I', expected_command, command_at + 28) == (0x44,)
    struct.pack_into('<I', expected_command, command_at + 28, 0x40)
    assert (nn_patches, command_patches) == (plan.descriptor_patches, plan.state_patches)

    payloads = [bytes(expected_nn), capture.mem_read(old[1]['cpu'], old[1]['bytes']),
                capture.mem_read(old[2]['cpu'], old[2]['bytes']) if old[2] else b'',
                bytes(expected_command), capture.mem_read(old[4]['cpu'], old[4]['bytes']),
                capture.mem_read(old[5]['cpu'], old[5]['bytes'])]
    for i, (buf, req, payload, raw) in enumerate(zip(buffers, plan.buffers, payloads, storage, strict=True)):
        if not buf.bytes:
            assert i == 2 and buf.cpu is None and buf.device == 0 and payload == b'' and raw is None
            continue
        pad = req.allocation_bytes - len(payload)
        expected = payload + (struct.pack('<II', 0x10000000, 0) * (pad // 8) if i == 3 else bytes(pad))
        actual = c.string_at(buf.cpu, buf.bytes)
        assert actual == expected, f'allocation {i} mismatch'
        assert c.string_at(buf.cpu - 4096, 4096) == b'\xa5' * 4096
        assert c.string_at(buf.cpu + buf.bytes, 4096) == b'\xa5' * 4096
        (args.output/f'buffer-{i}.bin').write_bytes(actual)
    report = dict(result='pass', nn_operations=plan.nn_operations,
                  descriptor_relocations=nn_patches, command_relocations=command_patches,
                  command_body_bytes=plan.command_body_bytes,
                  command_image_bytes=plan.buffers[3].payload_bytes,
                  command_allocation_bytes=plan.buffers[3].allocation_bytes,
                  total_allocation_bytes=sum(b.bytes for b in buffers),
                  buffers=[dict(device=hex(b.device), bytes=b.bytes) for b in buffers],
                  model_sha256=model_sha,
                  native_sha256=hashlib.sha256(args.native.read_bytes()).hexdigest(),
                  capture=str(args.capture.resolve()), sdk_executed=False,
                  physical_access=False, submission_authorized=False)
    (args.output/'comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
