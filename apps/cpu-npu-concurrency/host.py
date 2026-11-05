#!/usr/bin/env python3
"""Validate the fixed CPU/GEMM/NPU receipt; this module never opens a board."""
import argparse
import json
from pathlib import Path
import struct

MAGIC = 0x315550434e555043
BYTES = 1728
MASK = (1 << 64) - 1
FAIL_MAGIC = 0x314c494146555043
CAUSES = ((1, 'published level is not NORMAL'), (2, 'publication is stale'),
          (4, 'fewer than five channels seen'))


def decode_failure(raw):
    """Decode the record that a failed check returns in place of the receipt."""
    require(len(raw) == BYTES, 'Failure record must contain exactly 1728 bytes.')
    w = struct.unpack('<216Q', raw)
    require(w[:3] == (FAIL_MAGIC, 1, BYTES), 'Failure record header differs.')
    names = ('stage core job observed expected cell now_ticks start_ticks workload_ticks mode completed '
             'ack_mask sdk_closed').split()
    result = dict(zip(names, w[3:16]))
    result['chunks'] = list(w[16:24])
    thermal = dict(zip('cause level seen passive stale sequence now_us deadline_us start_us'.split(), w[24:33]))
    thermal['millicelsius'] = [x - (1 << 64) if x >> 63 else x for x in w[33:38]]
    thermal['causes'] = [text for bit, text in CAUSES if thermal['cause'] & bit]
    thermal['freshness_margin_us'] = thermal['deadline_us'] - thermal['now_us']
    result['thermal'] = thermal
    result['workload_us'] = result['workload_ticks'] / 24
    return result


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unit_sums():
    """Independent integer matrix multiplication, without the native formula."""
    result = []
    for cpu in range(8):
        total = 0
        for row in range(16):
            for col in range(16):
                value = 0
                for inner in range(16):
                    value += (row + cpu + 2 + inner) * (inner - col)
                total += (1 + 16 * row + col) * value
        result.append(total & MASK)
    return result


def decode(raw, *, mode, model_index=None, reference=None):
    require(len(raw) == BYTES, 'Receipt must contain exactly 1728 bytes.')
    if struct.unpack_from('<Q', raw)[0] == FAIL_MAGIC:
        raise ValueError('Check failed: ' + json.dumps(decode_failure(raw), sort_keys=True))
    words = struct.unpack('<216Q', raw)
    (magic, version, size, actual_mode, cores, edge, target, completed, start, stop,
     epoch, acks, closed, overlap, first_top, last_top) = words[:16]
    require((magic, version, size, actual_mode, cores, edge, epoch, acks, first_top, last_top) ==
            (MAGIC, 1, BYTES, mode, 255, 16, 1, 255, 1, 1), 'Receipt header differs.')
    require(0 < start < stop <= start + 122400000, 'Work exceeds five seconds plus drain bound.')
    sums = unit_sums()
    cpus = []
    for cpu in range(8):
        core, chunks, digest, first, last, stopped, error, ack = words[16+8*cpu:24+8*cpu]
        require(core == cpu and chunks > 0 and digest == (chunks*sums[cpu]) & MASK,
                f'CPU {cpu} identity, arithmetic count or digest differs.')
        require(start <= first <= last <= stop and (stopped, error, ack) == (epoch, 0, epoch),
                f'CPU {cpu} work or stop acknowledgment differs.')
        cpus.append({'core': cpu, 'chunks': chunks, 'digest': digest})
    if mode == 0:
        require((target, completed, closed, overlap) == (0, 0, 0, 0) and not any(words[80:]),
                'CPU control contains NPU activity.')
    else:
        require(mode == 1 and model_index is not None and reference is not None and
                0 < len(reference) <= 8, 'Combined receipt needs selected model and authenticated reference.')
        require((target, completed, closed, overlap) == (4, 4, 1, 15),
                'Combined jobs, close or overlap incomplete.')
        previous_irq = previous_ticket = 0
        previous_b = (0,) * 8
        tokens = set()
        for index in range(4):
            job = words[80+34*index:114+34*index]
            (ticket, token, client, model, trigger, busy_a, end_a, start_b, busy_b, irq,
             idle_a, idle_b, state_a, state_b) = job[:14]
            require(ticket > previous_ticket and token != 0 and client == 0 and model == model_index,
                    f'Job {index} ticket, token, client or selected model differs.')
            require(max(start, previous_irq) <= trigger <= busy_a <= end_a <= start_b <= busy_b < irq <= stop and
                    irq > previous_irq, f'Job {index} busy/counter/IRQ order differs.')
            require(idle_a != 0x7fffffff and idle_b != 0x7fffffff and
                    (state_a, state_b) == (3, 3), f'Job {index} lacks two RUNNING hardware busy samples.')
            before, after = job[14:22], job[22:30]
            require(all(previous_b[c] <= a < b <= cpus[c]['chunks'] for c, (a, b) in enumerate(zip(before, after))),
                    f'Job {index} lacks checked arithmetic from every CPU between busy samples.')
            output_bytes, matched, count, output = job[30:34]
            require((output_bytes, matched, count) == (len(reference), 1, 1) and
                    output.to_bytes(8, 'little') == reference.ljust(8, b'\0'),
                    f'Job {index} output or actual IRQ completion differs.')
            previous_irq, previous_ticket = irq, ticket
            previous_b = after
            tokens.add(token)
    elapsed_us = (stop-start)/24
    for cpu in cpus:
        cpu['units_per_second'] = cpu['chunks']*1000000/elapsed_us
        cpu['elapsed_us_per_unit'] = elapsed_us/cpu['chunks']
    result = {'mode': mode, 'core_mask': cores, 'ack_mask': acks, 'cores': cpus,
              'jobs': completed, 'overlap_mask': overlap, 'elapsed_us': elapsed_us,
              'cost_scope': 'Elapsed time per verified GEMM includes preemption; CPU cycles are not measured.'}
    if mode:
        latencies = [(words[80+34*i+9]-words[80+34*i+4])/24 for i in range(4)]
        ordered = sorted(latencies)
        window = words[80+34*3+9]-words[80+4]
        result['npu'] = {'samples': 4, 'trigger_to_irq_us': latencies,
                         'p50_us': ordered[1], 'p99_us': ordered[3], 'max_us': ordered[3],
                         'trigger_window_jobs_per_second': 4*24000000/window,
                         'scope': 'Four trigger-to-IRQ samples; nearest-rank quantiles, no thermal qualification.'}
    return result


def freeze_decoder(image, destination, mode):
    """Bind a standalone decode(payload) to this ELF's authenticated vectors."""
    import hashlib
    from elftools.elf.elffile import ELFFile
    with Path(image).open('rb') as stream:
        elf = ELFFile(stream)
        symbols = {s.name: (int(s['st_value']), int(s['st_size']))
                   for s in elf.get_section_by_name('.symtab').iter_symbols()
                   if s['st_shndx'] != 'SHN_UNDEF' and s['st_info']['bind'] == 'STB_GLOBAL'}
        segments = [(int(s['p_vaddr']), s.data()) for s in elf.iter_segments() if s['p_type'] == 'PT_LOAD']
    def read(address, size):
        for begin, data in segments:
            if begin <= address and address + size <= begin + len(data):
                return data[address-begin:address-begin+size]
        raise ValueError('Decoder binding reads outside initialized linked data.')
    def scalar(name):
        address, size = symbols[name]
        require(size in (4, 8), 'Decoder scalar layout differs.')
        return int.from_bytes(read(address, size), 'little')
    model_index, reference = None, None
    if mode == 1:
        # The image links no graph: the application carries the diagnostic's graph and registers it, so it is model 0.
        require('cpu_npu_graph' in symbols, 'Combined decoder requires the application to carry its NPU graph.')
        reference = read(symbols['cpu_npu_reference'][0], scalar('cpu_npu_reference_bytes'))
        require(0 < len(reference) <= 8, 'Linked reference does not fit the receipt.')
        model_index = 0
    else:
        require(mode == 0 and 'cpu_npu_graph' not in symbols, 'CPU control decoder cannot bind NPU work.')
    binding = {'image_sha256': hashlib.sha256(Path(image).read_bytes()).hexdigest(),
               'mode': mode, 'model_index': model_index,
               'reference_hex': reference.hex() if reference is not None else None}
    source = Path(__file__).read_text()
    # The wrapper has no source-relative imports or mutable external data.
    source += '\nBINDING = ' + repr(binding) + '\n_bound_decode = decode\n'
    source += 'def decode(payload):\n    return _bound_decode(payload, mode=BINDING["mode"], model_index=BINDING["model_index"], reference=bytes.fromhex(BINDING["reference_hex"]) if BINDING["reference_hex"] is not None else None)\n'
    Path(destination).write_text(source)
    return binding


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--freeze-image', type=Path)
    parser.add_argument('--decoder-output', type=Path)
    parser.add_argument('--payload', type=Path)
    parser.add_argument('--mode', type=int, choices=(0, 1), required=True)
    parser.add_argument('--model-index', type=int)
    parser.add_argument('--reference', type=Path)
    args = parser.parse_args()
    if args.freeze_image:
        require(args.decoder_output is not None, 'Frozen decoder output path required.')
        print(json.dumps(freeze_decoder(args.freeze_image, args.decoder_output, args.mode), sort_keys=True))
        return
    require(args.payload is not None, 'Receipt payload required.')
    result = decode(args.payload.read_bytes(), mode=args.mode, model_index=args.model_index,
                    reference=args.reference.read_bytes() if args.reference else None)
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
