#!/usr/bin/env python3
"""Check real software timers and explicit scheduler advances on eight cores."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import importlib.util
from pathlib import Path
from harness import available_gib

GATE_RUNS = [['--output', '{output}']]
CHECK_INPUTS = ['checks/qemu-os-boot.py', 'checks/qemu-os/clock-api-*',
                'kernel/scheduler/*', 'patches/freertos-*.patch', 'platform/qemu-os/*']


def run(output, tickless=False, negative=None):
    assert available_gib() is None or available_gib() >= 32, '32 GiB free required before build'
    root = Path(__file__).resolve().parents[1]
    spec = importlib.util.spec_from_file_location('clock_qemu', root/'checks/qemu-os-boot.py')
    qemu = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(qemu)
    qemu.ThreadPoolExecutor = lambda max_workers: ThreadPoolExecutor(max_workers=min(8, max_workers))
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)

    class ClockImage(qemu.Image):
        def prepare_kernel(self):
            kernel = super().prepare_kernel()
            if negative == 'object-lock':
                source = kernel/'queue.c'
                text = source.read_text()
                for call in ('ssKernelObjectSuspendOn( pxQueue );', '( void ) ssKernelObjectResumeOn();'):
                    assert text.count(call) == 1
                    text = text.replace(call, '')
                source.write_text(text)
            elif negative == 'indefinite':
                source = kernel/'tasks.c'
                text = source.read_text()
                expression = 'xWaitIndefinitely != pdFALSE ? portMAX_DELAY : xTicksToWait'
                assert text.count(expression) == 1
                source.write_text(text.replace(expression, 'xTicksToWait'))
            return kernel

    defines = ['-DSS_SCHED_CLOCK_TEST_TIMERS=1']
    sources = []
    if tickless:
        defines += ['-DSUNSTONE_OWNER_TICKLESS=1']
        sources += ['runtime/tickless.c', 'kernel/scheduler/tickless.c', 'kernel/scheduler/tickless_clock.c']
    image = ClockImage(output, extra_defines=defines, extra_sources=sources, partitioned=True)
    app = image.application('clock-api', ['checks/qemu-os/clock-api-main.c'],
                            root/'checks/qemu-os/clock-api-manifest.json')
    elf = image.build(app, wraps=['vApplicationIRQHandler'])
    raw, contract, _ = qemu.session_for(elf, None)
    log = output/'clock-api.log'
    text, status, seconds = qemu.boot(elf, raw, contract, qemu.scenario(limit_ms=8000), [], log)
    case = qemu.Case('scheduler-clock-apis', text, status, seconds, log)
    if negative:
        source = (root/'kernel/scheduler/freertos.c').read_text().splitlines()
        needle = ('configASSERT(self->o_depth);' if negative == 'object-lock' else
                  'configASSERT(ss_deadline_after(ssKernelNow(), (uint64_t)ticks, &deadline));')
        matches = [i + 1 for i, line in enumerate(source) if line.strip() == needle]
        assert len(matches) == 1
        line = matches[0]
        assert status == 2 and ('code=' + str(line)) in text and 'PANIC' in text, text[-3000:]
        print('PASS negative control: restricted timer wait rejects', negative)
    else:
        case.clean()
        case.require(case.finished is not None, 'timer/API application did not finish')
        data = bytes.fromhex(case.finished['result'])
        assert data == b'PASS scheduler explicit advance and auto-reload timer', data
        print('PASS eight-core software timer, additive CatchUp, CPU0 IncrementTick and 32-bit boundary; tickless=', tickless)
    return qemu, image, app[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--tickless', action='store_true')
    parser.add_argument('--negative', choices=['object-lock', 'indefinite'])
    args = parser.parse_args()
    run(args.output, args.tickless, args.negative)
    if not args.negative:
        for defect in ('object-lock', 'indefinite'):
            run(args.output/defect, args.tickless, defect)


if __name__ == '__main__':
    main()
