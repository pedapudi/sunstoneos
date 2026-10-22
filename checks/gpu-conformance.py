#!/usr/bin/env python3
"""Check the GPU conformance plan, the pinned-Mesa reference and the comparator before board time.

Builds pinned Mesa llvmpipe and the reference renderer (skipped when the stamp
is unchanged), then checks that:

- every closed-form scene, computed exactly in Python, matches the reference
  within its tolerance class, with no edge-tie pixels and no reference pixel
  within its tolerance of PVR_RENDER_POISON;
- the reference meets the profile 4 confirmation bound, and filter weights
  truncated to 2^-6 fail it;
- the traced closed-form stencil executions tell every stencil operation on
  every path, and every compare operation, apart from the other seven;
- each of the ten mutations fails at least one scene: five reference render
  parameters, coarse filter weights, two altered graphics captures and two
  altered compute captures;
- one scene violating each plan check is refused;
- the twelve launches meet their readback, frame-stream, session, slot and
  upload budgets, and the 1920 x 1080 launch is refused by the plan and by the
  inputs command;
- the capture comparison passes exact frames, reports coarse filter weights
  as unconfirmed, records a failed frame and refuses a wrong input hash,
  state or sequence;
- the comparator handles a 1920 x 384 frame within 50 ms;
- every feature row in docs/gpu-conformance.md has an output comparison;
- the checks after the Mesa build take at most 60 s.

The board is never touched; see docs/gpu-conformance.md.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('gpu_conformance', ROOT / 'tools/gpu-conformance.py')
gc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gc)
Q = gc.Q

PARAMETER_MUTATIONS = {1: 'half-pixel viewport offset', 2: 'other filter', 4: 'other interpolation mode',
                       8: 'mirrored repeat for mirror clamp to edge', 16: 'skipped sRGB decoding'}


def refused(build, reason_part):
    """Run build() and require a Refused whose message names reason_part."""
    try:
        build()
    except gc.Refused as error:
        if reason_part not in str(error):
            raise AssertionError(f'refused for another reason: {error}')
        return str(error)
    raise AssertionError(f'not refused: {reason_part}')


def refusal_scenes():
    """One scene violating each plan check, except the sentinel and timing
    checks, which the caller exercises with the reference and the launch model."""
    full = gc.pattern(255 * 129, 'p2-full')
    tw, th, tex = gc.profile4_texture()
    cases = {}
    cases['vertex lattice'] = (gc.Frame('refuse-lattice', 2, 255, 129, [gc.Batch([gc.Tri((
        (1 / 16, -1.0, 0.0, 1.0, 0.5, 0.0), (1.0, -1.0, 0.0, 1.0, 1.0, 0.0), (1.0, 1.0, 0.0, 1.0, 1.0, 1.0)))])],
        texels=(255, 129, full)), 'lattice')
    cases['texture coordinates'] = (gc.Frame('refuse-uv', 4, 128, 128, [gc.Batch([gc.Quad(
        0, 0, 64, 63, (Q(0), Q(9, 2)), (Q(0), Q(1)))], gc.depth_state(gc.ALWAYS))], state=1, texels=(tw, th, tex)),
        'magnitude 4')
    cases['nearest samples'] = (gc.Frame('refuse-nearest', 2, 255, 129, [gc.Batch([gc.Quad(
        0, 0, 255, 129, (Q(0), Q(1)), (Q(0), Q(1)))])], texels=(510, 258, gc.pattern(510 * 258, 'big'))),
        'texel boundary')
    near = Q(1, 2) + Q(1, 1 << 20)
    centre = (gc.texel_centre(0, 8),) * 2, (gc.texel_centre(0, 8),) * 2
    cases['depth'] = (gc.Frame('refuse-depth', 4, 128, 128, [
        gc.Batch([gc.Quad(0, 0, 64, 63, *centre, z=Q(1, 2))], gc.depth_state(gc.ALWAYS, True)),
        gc.Batch([gc.Quad(16, 16, 48, 47, *centre, z=near)], gc.depth_state(gc.LESS))], state=2, texels=(tw, th, tex)),
        'differ in depth')
    # Top edges run through the centres of rows r; nothing is strictly inside.
    thin = [gc.Tri(((-1.0, -1 + (2 * r + 1) / 64, 0.0, 1.0, 1, 1, 1, 1),
                    (1.0, -1 + (2 * r + 1) / 64, 0.0, 1.0, 1, 1, 1, 1),
                    (-1.0, -1 + (2 * r + 3) / 64, 0.0, 1.0, 1, 1, 1, 1))) for r in range(0, 60, 4)]
    cases['ambiguous pixels'] = (gc.Frame('refuse-ties', 1, 64, 64, [gc.Batch(thin)]), 'exceed 5%')
    cases['two inexact stages: linear sRGB'] = (gc.Frame('refuse-linear-srgb', 4, 128, 128, [gc.Batch([gc.Quad(
        0, 0, 64, 63)], gc.depth_state(gc.ALWAYS))], state=1 | 1 << 5 | 1 << 12, texels=(tw, th, tex)),
        'two inexact stages')
    translucent = (gc.texel_centre(4, 8),) * 2, (gc.texel_centre(7, 8),) * 2
    cases['two inexact stages: blend over blend'] = (gc.Frame('refuse-double-blend', 4, 128, 128, [
        gc.Batch([gc.Quad(0, 0, 64, 63, *translucent)], gc.depth_state(gc.ALWAYS) | gc.blend_state(1)),
        gc.Batch([gc.Quad(16, 16, 48, 47, *translucent)], gc.depth_state(gc.ALWAYS) | gc.blend_state(3))],
        state=2, texels=(tw, th, tex)), 'one blended layer')
    return cases


def stride_mutation(capture, width, height, stride):
    """Read the padded capture as if rows had no padding."""
    out = bytearray(capture)
    for y in range(height):
        out[4 * y * stride:4 * (y * stride + width)] = capture[4 * y * width:4 * (y * width + width)]
    return bytes(out)


def feature_rows():
    text = (ROOT / 'docs/gpu-conformance.md').read_text()
    section = text.split('## Coverage', 1)[1].split('\n## ', 1)[0]
    rows = []
    for line in section.splitlines():
        if line.startswith('| ') and not line.startswith('| Feature') and not line.startswith('| ---'):
            rows.append(line.split('|')[1].strip())
    return rows


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--output', type=Path, required=True, help='new directory for results')
    p.add_argument('--build', type=Path, default=gc.BUILD)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    result = {'build': gc.build(a.build)}
    result['build_repeat'] = gc.build(a.build)
    assert not result['build_repeat']['mesa_rebuilt'], 'unchanged stamp rebuilt Mesa'
    # The 60-second budget is defined with Mesa already built.
    start = time.monotonic()
    result['build_s'] = round(start - started, 2)
    processes = 0

    # Plan, budgets and launch refusal, on the plan and on the inputs path.
    planned = gc.plan()
    assert len(planned['launches']) == 12
    assert planned['readback_bytes'] <= 8 << 20
    result['plan'] = planned
    result['refusals'] = {'timing: 1920 x 1080 launch': refused(lambda: gc.plan(gc.refused_launches()), 'exceeds')}
    result['refusals']['inputs: 1920 x 1080 launch'] = refused(
        lambda: gc.write_inputs(gc.refused_launches()[0], a.output / 'refused-inputs'), 'exceeds')
    assert not (a.output / 'refused-inputs').exists()

    # Process A: closed-form scenes, reference mutations and capture mutations.
    launch_list = gc.launches()
    board = [f for l in launch_list if l.frames for f in l.frames]
    closed = [f for f in board if f.closed_form] + gc.colour_host_frames()
    entries = []
    trace = set()
    for frame in closed:
        analysis = gc.analyze(frame)
        assert analysis.tie_pixels == 0, f'{frame.name}: closed-form scene has edge ties'
        entries.append((frame, analysis, gc.closed_form(frame, trace=trace), 31))
    copy = next(i for i, e in enumerate(entries) if e[0].name == 'p2-copy')
    frame, analysis, capture, _ = entries[copy]
    entries.append((frame, analysis, stride_mutation(capture, frame.width, frame.height, frame.stride), 0))
    changed = bytearray(capture)
    pixel = 4 * (50 * frame.stride + 100)
    changed[pixel] = (changed[pixel] + 2) % 256
    entries.append((frame, analysis, bytes(changed), 0))
    info, libs, lines, rendered = gc.reference(entries, a.output, a.build, frames=True)
    processes += 1
    result['renderer'] = info
    result['libraries'] = libs
    gc.check_sentinels(lines, entries)
    verdicts = gc.judge(lines, entries)
    n = len(closed)
    base = [v for v, l in zip(verdicts, lines) if l['mutation'] == 0 and l['frame'] < n]
    failed = [v['frame'] for v in base if not v['passed']]
    assert not failed, f'closed-form scenes differ from the reference: {failed}'
    mutations = {}
    for bit, name in PARAMETER_MUTATIONS.items():
        caught = [v['frame'] for v, l in zip(verdicts, lines) if l['mutation'] == bit and not v['passed']]
        assert caught, f'mutation not detected: {name}'
        mutations[name] = caught
    capture_lines = [v for v, l in zip(verdicts, lines) if l['frame'] >= n]
    assert len(capture_lines) == 2 and not any(v['passed'] for v in capture_lines)
    mutations['stride padding read as pixels'] = [capture_lines[0]['failed']]
    mutations['one exact pixel changed by 2'] = [capture_lines[1]['failed']]
    result['closed_form'] = [{k: v[k] for k in ('frame', 'covered', 'exact', 'tolerant', 'failed', 'sentinel',
                                                'compare_ms')} for v in base]

    # Confirmation scenes: the exact frame and the reference meet the bound,
    # and filter weights truncated to 2^-6 do not.
    confirmed = {}
    images = {}
    for (frame, _, exact, _), image in zip(entries, rendered):
        pixels = gc.confirmation_pixels(frame)
        if not pixels or frame.name in confirmed:
            continue
        assert not gc.confirm(frame, exact, frame.stride), f'{frame.name}: exact frame'
        failures = gc.confirm(frame, image, frame.width)
        assert not failures, f'{frame.name}: reference confirmation pixels {failures[:4]}'
        confirmed[frame.name] = len(pixels)
        images[frame.name] = exact
        if frame.state & 32:
            coarse = gc.closed_form(frame, weight_bits=6)
            if gc.confirm(frame, coarse, frame.stride):
                images[frame.name + ' coarse'] = coarse
    caught = [name for name in images if name.endswith(' coarse')]
    assert len(confirmed) == 12 and len(caught) == 6, (confirmed, caught)
    mutations['filter weights truncated to 2^-6'] = caught
    result['confirmation_pixels'] = confirmed

    # Stencil: every operation on every path, and every compare operation, is
    # told apart from the other seven by some traced execution.
    missing, compare_mask, write_mask = gc.stencil_separation(trace)
    assert not missing, f'stencil behaviour not separated: {missing}'
    assert compare_mask and write_mask, 'a stencil mask changes no traced outcome'

    # Compute: closed-form expressions, two capture mutations, sentinel refusal.
    compute = {}
    for launch in launch_list:
        if launch.kind != 'compute':
            continue
        k = launch.kernel
        expected = gc.compute_plan(k)
        good = [{'input_sha256': hashlib.sha256(d).hexdigest(), 'result': 1, 'job_error': 0, 'words': w}
                for d, w in zip(k.inputs, expected)]
        assert gc.compare_compute(k, expected, good)['passed']
        bad = [dict(x) for x in good]
        bad[1]['words'] = list(bad[1]['words'])
        bad[1]['words'][7] ^= 1
        r = gc.compare_compute(k, expected, bad)
        assert not r['passed'] and r['inputs'][1]['failed'] == 1
        job = [dict(x) for x in good]
        job[2].update(result=2, phase='READ_OUTPUT', job_error=gc.job_error_result(), words=[])
        r = gc.compare_compute(k, expected, job)
        assert not r['passed'] and r['inputs'][2]['status'].startswith('out-of-range write')
        compute[k.name] = {'inputs': len(k.inputs), 'words': len(expected[0])}
    mutations['one compute word changed'] = list(compute)
    mutations['compute job error PVR_JOB_RESULT'] = list(compute)
    assert len(mutations) == 10
    result['mutations'] = mutations
    result['compute'] = compute
    affine = next(l.kernel for l in launch_list if l.kernel and l.kernel.name == 'vector-affine')
    sentinel = gc.Kernel('sentinel', affine.source, 1, 12, 4, 4,
                         [struct.pack('<5I', 2, 1, 0, 0xa5a5a5a5, 5)], affine.expected)
    result['refusals']['sentinel: compute word'] = refused(lambda: gc.compute_plan(sentinel), 'output fill')

    # Plan refusals that need no reference.
    for name, (frame, reason) in refusal_scenes().items():
        result['refusals'][name] = refused(lambda: gc.analyze(frame), reason)

    # Process B: every other board frame against itself, for sentinels and
    # comparator time.
    others = [f for f in board if not f.closed_form]
    entries = [(f, gc.analyze(f), 'self', 0) for f in others]
    info, libs, lines, _ = gc.reference(entries, a.output, a.build)
    processes += 1
    gc.check_sentinels(lines, entries)
    verdicts = gc.judge(lines, entries)
    assert all(v['passed'] for v in verdicts)
    wide = next(v for v in verdicts if v['frame'] == 'p4-wide-1920x384')
    assert wide['compare_ms'] <= 50, f'comparator took {wide["compare_ms"]} ms on 1920 x 384'
    result['board_scenes'] = [{k: v[k] for k in ('frame', 'covered', 'ties', 'compare_ms')} for v in verdicts]
    result['comparator_ms_1920x384'] = wide['compare_ms']

    # Process C: a texture whose copy equals PVR_RENDER_POISON.
    poison = bytearray(gc.pattern(255 * 129, 'p2-full'))
    poison[4 * 1000:4 * 1001] = struct.pack('<I', gc.POISON)
    frame = gc.Frame('refuse-sentinel', 2, 255, 129, [gc.Batch([gc.Quad(0, 0, 255, 129, (Q(0), Q(1)), (Q(0), Q(1)))])],
                     texels=(255, 129, bytes(poison)))
    entries = [(frame, gc.analyze(frame), 'self', 0)]
    _, _, lines, _ = gc.reference(entries, a.output, a.build)
    processes += 1
    result['refusals']['sentinel: reference pixel'] = refused(lambda: gc.check_sentinels(lines, entries), 'POISON')

    # Process D: the capture comparison path, with synthesized profile 4
    # captures. Only the first comparison renders; the others stop before
    # the reference.
    result['capture_comparison'] = capture_comparisons(a.output, a.build, images)
    processes += 1
    clear = a.output / 'clear-capture'
    clear.mkdir()
    (clear / 'clear.rgba').write_bytes(struct.pack('<I', gc.CLEAR) * 4096)
    (clear / 'capture.json').write_text(json.dumps({'frames': [{'file': 'clear.rgba'}]}))
    assert gc.compare_capture('fixed-clear', clear, a.output / 'clear-comparison')['passed']
    (clear / 'clear.rgba').write_bytes(struct.pack('<I', gc.CLEAR) * 4095 + struct.pack('<I', gc.POISON))
    assert not gc.compare_capture('fixed-clear', clear, a.output / 'clear-mutated')['passed']
    shutil.rmtree(clear)

    # Coverage: every documented feature row has a comparison.
    covered = gc.coverage(closed, board, [l.kernel for l in launch_list if l.kernel], trace)
    missing = [row for row in feature_rows() if not covered.get(row)]
    extra = [row for row in covered if row not in feature_rows()]
    assert not missing and not extra, f'coverage table differs: missing {missing}, undocumented {extra}'
    result['coverage'] = covered
    result['reference_processes'] = processes
    assert processes <= 5
    result['wall_s'] = round(time.monotonic() - start, 2)
    assert result['wall_s'] <= 60, f'host check took {result["wall_s"]} s after the build'
    (a.output / 'result.json').write_text(json.dumps(result, indent=1) + '\n')
    print(json.dumps({'passed': True, 'build_s': result['build_s'], 'wall_s': result['wall_s'],
                      'reference_processes': processes, 'comparator_ms_1920x384': result['comparator_ms_1920x384'],
                      'worst_frame_stream_s': planned['worst_frame_stream_s'],
                      'worst_session_s': planned['worst_session_s'], 'readback_bytes': planned['readback_bytes'],
                      'mutations': len(mutations), 'refusals': len(result['refusals']),
                      'features': len(covered)}))


def capture_comparisons(output, build, images):
    """Run compare_capture on synthesized profile 4 captures: exact frames
    pass and confirm; coarse filter weights leave the launch unconfirmed; a
    failed frame is recorded with its result; and a wrong input hash, state
    or sequence is refused. images maps a frame name to its closed-form
    capture, and 'name coarse' to the capture with coarse filter weights.
    Returns each case's status."""
    launch = gc.find_launch('profile4')
    inputs = output / 'profile4-inputs'
    gc.write_inputs(launch, inputs)
    capture = output / 'profile4-capture'
    capture.mkdir()
    frames = []
    for k, frame in enumerate(launch.frames):
        (capture / f'frame-{k}.rgba').write_bytes(images.get(frame.name) or gc.closed_form(frame))
        data = (inputs / f'frame-{k}.input').read_bytes()
        frames.append({'input_sha256': hashlib.sha256(data).hexdigest(), 'state': frame.state,
                       'sequence': k + 1, 'generation': 1, 'result': 1, 'file': f'frame-{k}.rgba'})

    def compare(case, records):
        (capture / 'capture.json').write_text(json.dumps({'launch': 'profile4', 'frames': records}))
        return gc.compare_capture('profile4', capture, output / f'profile4-{case}', build)
    statuses = {}
    report = compare('exact', frames)
    assert report['passed'] and report['status'] == 'passed', report['status']
    statuses['exact frames'] = report['status']
    for k, frame in enumerate(launch.frames):
        if frame.state & 32:
            (capture / f'frame-{k}.rgba').write_bytes(images[frame.name + ' coarse'])
    report = compare('coarse', frames)
    assert not report['passed'] and report['status'].startswith('unconfirmed'), report['status']
    statuses['filter weights truncated to 2^-6'] = report['status']
    report = compare('failed', [dict(frames[0], result=3)])
    assert not report['passed'] and report['frames'][0]['status'] == 'failed at result 3'
    assert report['status'].startswith('unconfirmed'), report['status']
    statuses['first frame failed with result 3'] = report['frames'][0]['status']
    for case, change, reason in (('hash', {'input_sha256': '0' * 64}, 'hash differs'),
                                 ('state', {'state': frames[3]['state'] ^ 1}, 'state'),
                                 ('sequence', {'sequence': 9}, 'sequence')):
        records = [dict(f) for f in frames]
        records[3].update(change)
        statuses[f'wrong {case}'] = refused(lambda: compare(f'wrong-{case}', records), reason)
    shutil.rmtree(inputs)
    shutil.rmtree(capture)
    return statuses


if __name__ == '__main__':
    sys.dont_write_bytecode = True
    main()
