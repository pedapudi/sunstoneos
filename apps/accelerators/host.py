#!/usr/bin/env python3
"""Interleave retained GPU compute and complete NPU graph calls on one image.

The coordinator supplies an exchange function and an authenticated session. This
module does not discover, open, launch, reset or recover a device. Existing wire
formats and their response validators remain the component contracts.
"""
import hashlib
import importlib.util
import math
from pathlib import Path
import time


def _component(name, relative):
    path = Path(__file__).resolve().parent.parent / relative
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


gpu = _component('accelerator_gpu_runtime', 'gpu-program/runtime.py')
npu = _component('accelerator_npu_runtime', 'npu-runtime-large/host.py')


def sequence(exchange, prepared, gpu_words, gpu_input, npu_inputs, model, *, deadline):
    """Copy GPU1/NPU1/GPU2/NPU2 results, then close both native services.

    exchange(request, response_capacity, remaining_seconds) serializes a complete
    request/response using the coordinator's existing retained device handle.
    deadline is the coordinator's original absolute monotonic session end; no
    additional lifetime starts here. Inputs are raw caller-selected bytes.
    """
    if not math.isfinite(deadline) or deadline <= time.monotonic():
        raise ValueError('Expired combined session')
    if not 0 < gpu_words <= 992 or not 4 <= len(gpu_input) <= 4096:
        raise ValueError('GPU copied extent differs')
    if len(npu_inputs) != 2 or model.get('family') != 'nn-pool':
        raise ValueError('Two inputs for the retained complete NN graph are required')
    npu.validate_inputs(npu_inputs, [], model)
    counter = 0

    def transaction(request, capacity):
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError('Combined session deadline')
        raw = exchange(request, capacity, left)
        if time.monotonic() >= deadline:
            raise TimeoutError('Combined session expired during exchange')
        return raw

    def gpu_call(operation, expected, first=0, data=b''):
        raw = transaction(gpu.frame(operation, expected, prepared['epoch'], first, data), 168)
        return gpu.decode(raw, prepared['epoch'], gpu_words, first // 32)

    def gpu_output(expected):
        parts = []
        for first in range(0, gpu_words, 32):
            while True:
                state, actual, part = gpu_call(gpu.READ, expected, first)
                if state == 1:
                    if actual != expected:
                        raise ValueError('GPU output sequence changed')
                    parts.append(part)
                    break
                if state != 0:
                    raise ValueError('GPU closed before copied output')
                time.sleep(.001)
        return b''.join(parts)

    def npu_call(operation, rid, data=b'', offset=0, timeout_us=0, total=0):
        nonlocal counter
        counter += 1
        request = npu.packet(counter, operation, prepared, rid, timeout_us, data, offset, total)
        value = npu.decode(transaction(request, 1040), counter, prepared, rid)
        if value['state'] == npu.FAILED or value['model_sha256'] != model['model']['sha256']:
            raise ValueError('NPU retained or model identity differs')
        return value

    def npu_output(rid, data):
        value = npu_call(npu.BEGIN, rid, timeout_us=5000000, total=len(data))
        if value['state'] != npu.UPLOADING or value['received']:
            raise ValueError('NPU input begin differs')
        for offset in range(0, len(data), npu.CHUNK):
            part = data[offset:offset + npu.CHUNK]
            value = npu_call(npu.UPLOAD, rid, part, offset)
            if value['state'] != npu.UPLOADING or value['received'] != offset + len(part):
                raise ValueError('NPU copied input extent differs')
        value = npu_call(npu.COMMIT, rid)
        parts, offset, identity = [], 0, None
        while offset < model['reference']['bytes']:
            value = npu_call(npu.READ, rid, offset=offset)
            if value['state'] in (npu.QUEUED, npu.RUNNING):
                time.sleep(.001)
                continue
            if value['state'] != npu.DONE or value['total'] != model['reference']['bytes'] or value['offset'] != offset or not value['output']:
                raise ValueError('NPU copied output extent differs')
            current = (value['job'], value['jobs'], value['irq_completions'], value['input_sha256'], value['output_sha256'])
            if (identity is not None and identity != current) or value['jobs'] != rid or value['input_sha256'] != hashlib.sha256(data).hexdigest():
                raise ValueError('NPU output identity changed')
            identity = current
            parts.append(value['output'])
            offset += len(value['output'])
        output = b''.join(parts)
        if hashlib.sha256(output).hexdigest() != value['output_sha256']:
            raise ValueError('NPU copied output digest differs')
        npu_call(npu.FINISH, rid)
        return output

    first_gpu = gpu_output(1)
    first_npu = npu_output(1, npu_inputs[0])
    state, actual, _ = gpu_call(gpu.SUBMIT, 1, data=gpu_input)
    if (state, actual) != (0, 1):
        raise ValueError('GPU input acceptance differs')
    second_gpu = gpu_output(2)
    second_npu = npu_output(2, npu_inputs[1])
    state, actual, _ = gpu_call(gpu.CLOSE, 2)
    if (state, actual) != (0, 2):
        raise ValueError('GPU CLOSE acceptance differs')
    while True:
        state, actual, _ = gpu_call(gpu.READ, 2)
        if (state, actual) == (3, 2):
            break
        if state != 0:
            raise ValueError('GPU healthy shutdown differs')
        time.sleep(.001)
    value = npu_call(npu.CLOSE, 3, timeout_us=5000000)
    while value['state'] != npu.CLOSED:
        time.sleep(.001)
        value = npu_call(npu.READ, 3)
    if value['jobs'] != 2:
        raise ValueError('NPU healthy shutdown count differs')
    return dict(gpu_outputs=[first_gpu, second_gpu], npu_outputs=[first_npu, second_npu],
                gpu_closed=True, npu_closed=True)
