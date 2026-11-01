"""Decode the native CPU latency report without accessing a device."""
import struct

MAGIC = 0x53534C4154454E31
METRICS = ('timer_irq', 'irq_masked', 'lock0_wait', 'lock0_hold', 'lock1_wait',
           'lock1_hold', 'wake_idle', 'wake_busy', 'dma', 'gpu', 'npu')


def inputs():
    return []


def decode(payload, dma=False):
    if len(payload) != 5440:
        raise ValueError('latency result must contain 5440 bytes')
    magic, frequency, elapsed, mask, model, dma_jobs, irq_mask = struct.unpack_from('<3Q4I', payload)
    if magic != MAGIC or mask != 0xff or model or irq_mask != 1 or \
            (dma_jobs < 2 if dma else dma_jobs != 0):
        raise ValueError('native CPU latency header differs')
    if not frequency or not 0 < elapsed <= 10000:
        raise ValueError('latency counter frequency or run interval is invalid')
    cores = []
    fields = ('count', 'max_ticks', 'max_start', 'max_site', 'first_over_start',
              'first_over_ticks', 'first_over_site')
    for core in range(8):
        sweeps, read, written, digest = struct.unpack_from('<4Q', payload, 40 + core * 32)
        if sweeps < 2 or read != sweeps * 1048576 or written != read:
            raise ValueError(f'core {core} memory traffic is incomplete')
        samples = {}
        for metric, name in enumerate(METRICS):
            words = struct.unpack_from('<7Q', payload, 320 + core * 640 + metric * 56)
            samples[name] = dict(zip(fields, words))
            count, maximum, start, site, over_start, over_ticks, over_site = words
            if not count and any(words):
                raise ValueError(f'core {core} {name} has data without samples')
            if over_ticks > maximum or (not over_ticks and (over_start or over_site)):
                raise ValueError(f'core {core} {name} overrun context differs')
        if samples['wake_idle']['count'] != 64 or samples['wake_busy']['count'] != 128:
            raise ValueError(f'core {core} wake samples are incomplete')
        if any(samples[name]['count'] for name in ('gpu', 'npu')) or \
                (not dma and samples['dma']['count']) or \
                (dma and bool(samples['dma']['count']) != (core == 0)):
            raise ValueError('native CPU image cannot report accelerator or DMA samples')
        if dma and core == 0 and samples['dma']['count'] < 2:
            raise ValueError('native DMA completion coverage is incomplete')
        if bool(samples['timer_irq']['count']) != (core == 0):
            raise ValueError('timer IRQ coverage differs from CPU0-only delivery')
        cores.append({'core': core, 'sweeps': sweeps, 'bytes_read': read,
                      'bytes_written': written, 'digest': digest, 'metrics': samples})
    return {'model': False, 'frequency': frequency, 'elapsed_ticks': elapsed,
            'core_mask': mask, 'timer_irq_core_mask': irq_mask, 'dma_jobs': dma_jobs, 'cores': cores}
