#!/usr/bin/env python3
"""Build and run the CE probe state machine against fake operations (checks/ce-probe-core.c)."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='ce-probe-') as directory:
    binary = pathlib.Path(directory) / 'ce-probe-core'
    build = subprocess.run(['/usr/bin/clang-21', '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-I' + str(root / 'include'),
                            str(root / 'checks/ce-probe-core.c'), str(root / 'drivers/crypto/a733_ce_probe.c'),
                            '-o', str(binary)], capture_output=True, text=True)
    if build.returncode:
        sys.exit(build.stdout + build.stderr)
    record = pathlib.Path(directory) / 'record.bin'
    run = subprocess.run([str(binary), str(record)], capture_output=True, text=True)
    sys.stdout.write(run.stdout + run.stderr)
    if run.returncode or 'PASS' not in run.stdout:
        sys.exit(1)
    # The host decoder reads the record the C state machine wrote.
    sys.path.insert(0, str(root / 'tools'))
    import ce_probe_result
    decoded = ce_probe_result.decode(record.read_bytes())
    assert decoded['stage'] == 'done' and not decoded['failed'] and decoded['found_enabled_and_cycled']
    assert decoded['health_tests_at_hardware_defaults_not_programmed'] and decoded['block_not_all_zero']
    assert decoded['double_entropy_source_set'] and decoded['bytes_returned'] == 32 and decoded['step_limit_us'] == 2000000
    assert decoded['entry'] == {'ce_clk': 0x81000000, 'ce_bgr': 0x30003, 'mbus': 0x40306}
    assert decoded['descriptor_physical'] == 0x41000000 and decoded['output_physical'] == 0x41000100
    print('PASS CE probe record decodes on the host')
