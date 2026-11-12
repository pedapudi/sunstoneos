#!/usr/bin/env python3
"""Check audio parent retirement, exact shared-clock exclusion and faults."""
import argparse
import json
from pathlib import Path
import subprocess
from harness import CLANG, SANITIZE, environment

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--output', '{output}']]
SOURCES = ['checks/audio-parent-return.c', 'drivers/audio/a733_i2s_platform.c',
           'drivers/audio/a733_i2s.c', 'drivers/audio/a733_i2s_stream.c',
           'drivers/audio/a733_audio_leases.c', 'drivers/clock/a733_clock_leases.c',
           'drivers/pinctrl/a733_pins.c', 'drivers/pinctrl/a733_pin_tables.c',
           'drivers/dma/a733_dma.c', 'drivers/dma/a733_dma_controller.c']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    env = environment()
    env['TMPDIR'] = str(out)
    command = [str(CLANG), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
               *SANITIZE, '-I' + str(ROOT / 'include')]
    exe = out / 'audio-parent-return'
    result = subprocess.run(command + [str(ROOT / f) for f in SOURCES] + ['-o', str(exe)],
                            env=env, capture_output=True, text=True)
    (out / 'build.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stderr
    result = subprocess.run([str(exe)], env=env, capture_output=True, text=True)
    (out / 'run.log').write_text(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout + result.stderr
    print(result.stdout, end='')
    mutants = {
        'skip-cold-restoration': ('drivers/audio/a733_i2s_platform.c',
            'update(p,d,AUDIO,RATE_MASK,p->audio_value);', '1;'),
        'ignore-shared-consumer': ('drivers/clock/a733_clock_leases.c',
            'if((A733_CLOCK_AUDIO_CHANGE&(t->shared|t->exclusive)) ||',
            'if((0 && (A733_CLOCK_AUDIO_CHANGE&(t->shared|t->exclusive))) ||'),
    }
    for name, (source, old, new) in mutants.items():
        text = (ROOT / source).read_text()
        assert old in text
        mutant = out / (name + '.c')
        mutant.write_text(text.replace(old, new, 1))
        binary = out / name
        inputs = [str(mutant) if f == source else str(ROOT / f) for f in SOURCES]
        result = subprocess.run(command + inputs + ['-o', str(binary)], env=env, capture_output=True, text=True)
        (out / (name + '-build.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, result.stderr
        result = subprocess.run([str(binary)], env=env, capture_output=True, text=True)
        (out / (name + '-run.log')).write_text(result.stdout + result.stderr)
        assert result.returncode != 0, name + ': unsafe retirement accepted'
    (out / 'result.json').write_text(json.dumps({'passed': True, 'physical_access': False,
        'partial_cold_states': 16, 'consumer_conflicts': 5, 'mutants_rejected': list(mutants),
        'command': command + SOURCES}, indent=2) + '\n')


if __name__ == '__main__':
    main()
