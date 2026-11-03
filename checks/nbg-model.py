#!/usr/bin/env python3
"""Host wrapper over the native NBG decoder (drivers/npu/nbg_*.c), used by the NPU host tools.

The former driver that reconstructed captured SDK submissions with it was removed together with
checks/nbg-loader-reference.py; its result is recorded in docs/npu.md.
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
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class Section(c.Structure):
    _fields_ = [('offset', c.c_uint32), ('bytes', c.c_uint32)]


class Span(c.Structure):
    _fields_ = [('data', c.c_void_p), ('bytes', c.c_size_t)]


class Header(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in (
        'version', 'target', 'flags', 'core_count', 'checksum', 'layer_count',
        'operation_count', 'input_count', 'output_count', 'nn_bytes', 'tp_bytes',
        'pool_bytes', 'pool_alignment', 'pool_base', 'axi_sram_base', 'axi_sram_bytes',
        'vip_sram_base', 'vip_sram_bytes')] + [('name', c.c_char * 65)]


class View(c.Structure):
    _fields_ = [('data', c.c_void_p), ('bytes', c.c_size_t), ('header', Header),
                ('sections', Section * 40)]


class Tensor(c.Structure):
    _fields_ = [('dimensions', c.c_uint32), ('sizes', c.c_uint32 * 6),
                ('format', c.c_uint32), ('buffer_type', c.c_uint32),
                ('quantization', c.c_uint32), ('fixed_position', c.c_int32),
                ('scale_bits', c.c_uint32), ('zero_point', c.c_int32),
                ('logical_bytes', c.c_uint32), ('name', c.c_char * 65)]


class Operation(c.Structure):
    _fields_ = [(name, c.c_uint32) for name in
                ('kind', 'index', 'layer', 'state', 'first_patch', 'patch_count')]
    _fields_ += [('auxiliary', c.c_uint32 * 2)]


class Patch(c.Structure):
    _fields_ = [('kind', c.c_uint32), ('destination', c.c_uint32), ('source', c.c_uint32),
                ('index', c.c_int32)]
    _fields_ += [(name, c.c_uint32) for name in ('source_offset', 'transformed', 'name_word',
                                               'high_destination', 'high_mask', 'destination_base')]


class Native:
    def __init__(self, path, data):
        self.lib = c.CDLL(str(path.resolve()))
        self.data = c.create_string_buffer(data, len(data))
        self.view = View()
        self.lib.nbg_model_open.argtypes = [c.c_void_p, c.c_size_t, c.POINTER(View)]
        self.lib.nbg_model_section.argtypes = [c.POINTER(View), c.c_int, c.POINTER(Span)]
        self.lib.nbg_model_tensor.argtypes = [c.POINTER(View), c.c_int, c.c_uint32, c.POINTER(Tensor)]
        self.lib.nbg_model_operation.argtypes = [c.POINTER(View), c.c_uint32, c.POINTER(Operation)]
        self.lib.nbg_model_patch.argtypes = [c.POINTER(View), c.c_uint32, c.POINTER(Patch)]
        self.lib.nbg_model_table.argtypes = [c.POINTER(View), c.c_int, c.c_uint32, c.POINTER(Span)]
        self.table_calls = 0
        if self.lib.nbg_model_open(self.data, len(data), c.byref(self.view)) != 0:
            raise RuntimeError('native decoder refused pinned fixture')

    def section(self, index):
        span = Span()
        if self.lib.nbg_model_section(c.byref(self.view), index, c.byref(span)) != 1:
            raise RuntimeError('native section accessor refused')
        return c.string_at(span.data, span.bytes)

    def table_entries(self, index):
        target = {4: 5, 22: 23, 24: 18, 25: 19, 26: 20, 13: 14}[index]
        base = self.view.data + self.view.sections[target].offset
        result = []
        for row in range(self.view.sections[index].bytes // 8):
            span = Span()
            if self.lib.nbg_model_table(c.byref(self.view), index, row, c.byref(span)) != 1:
                raise RuntimeError('native table accessor refused')
            result.append((span.data - base, span.bytes))
            self.table_calls += 1
        return result

    def operations(self):
        result = []
        for index in range(self.view.header.operation_count):
            row = Operation()
            if self.lib.nbg_model_operation(c.byref(self.view), index, c.byref(row)) != 1:
                raise RuntimeError('native operation accessor refused')
            result.append(tuple(getattr(row, name) for name, _ in Operation._fields_[:-1]) + tuple(row.auxiliary))
        return result

    def patches(self):
        result = []
        width = 28 if self.view.header.version == 0x20000 else 40
        for index in range(self.view.sections[9].bytes // width):
            row = Patch()
            if self.lib.nbg_model_patch(c.byref(self.view), index, c.byref(row)) != 1:
                raise RuntimeError('native patch accessor refused')
            result.append(tuple(getattr(row, name) for name, _ in Patch._fields_))
        return result

    def tensors(self, section, count):
        result = []
        for index in range(count):
            row = Tensor()
            if self.lib.nbg_model_tensor(c.byref(self.view), section, index, c.byref(row)) != 1:
                raise RuntimeError('native tensor accessor refused')
            result.append({'dimensions': list(row.sizes)[:row.dimensions],
                           'logical_bytes': row.logical_bytes, 'name': row.name.decode(),
                           'buffer_type': row.buffer_type, 'format': row.format,
                           'quantization': row.quantization, 'scale_bits': row.scale_bits,
                           'fixed_position': row.fixed_position, 'zero_point': row.zero_point})
        return result


class Capture:
    """Only byte reads from recorded snapshots; no CPU emulator is constructed."""
    def __init__(self, directory, sdk_sha, model_sha, *, require_comparison=True):
        manifest = json.loads((directory.parent/'manifest.json').read_text())
        for path in directory.rglob('*'):
            if path.is_file():
                identity = manifest.get(str(path.resolve()))
                if (identity is None or path.stat().st_size != identity['bytes'] or
                    hashlib.sha256(path.read_bytes()).hexdigest() != identity['sha256']):
                    raise RuntimeError(f'changed or unrecorded capture artifact: {path}')
        self.report = json.loads((directory/'trace.json').read_text())
        if (self.report['sdk_sha256'] != sdk_sha or self.report['model_sha256'] != model_sha or
            self.report['result'] != 'submission_intercepted' or
            self.report.get('error') is not None or self.report.get('physical_access') is not False or
            (require_comparison and (self.report.get('comparison') or {}).get('result') != 'pass')):
            raise RuntimeError('capture identities or reference result do not match')
        self.allocations, self.tasks, self.bindings = (
            self.report[key] for key in ('allocations', 'tasks', 'bindings'))
        self.uc = self
        self.regions = [(self.report['network'], (directory/'submission/network.bin').read_bytes())]
        for allocation in self.allocations:
            if allocation['video']:
                data = (directory/'submission'/f"buffer-{allocation['handle']}.bin").read_bytes()
                if len(data) != allocation['bytes']:
                    raise RuntimeError('capture allocation length mismatch')
                self.regions.append((allocation['cpu'], data))

    def mem_read(self, address, size):
        for base, data in self.regions:
            if base <= address <= base + len(data) and size <= base + len(data) - address:
                return data[address - base:address - base + size]
        raise RuntimeError('reference snapshot read outside captured buffers')

    def read64(self, address):
        return struct.unpack('<Q', self.mem_read(address, 8))[0]
