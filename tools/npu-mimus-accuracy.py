#!/usr/bin/env python3
"""Score the NPU INT16 arithmetic of mimus linear layers on held-out speech.

Runs in the mimus training environment against a mimus checkout that holds
train/static_scales.py. It reuses that study's model, calibration, decoding
and result format, and changes only the arithmetic of every 16-bit site
whose weights are int8 codes, to what the compiled INT16 NPU layer computes
(docs/npu-mimus-linear.md):

  input    one static scale per site, s = T / 32767 with T the calibrated
           |x| statistic chosen by --calib; codes clamp(rne(x / s), +-32767).
  sum      the exact integer sum S[n] over the int8 weight codes.
  bias     one int32 code per output channel, rne(bias[n] / (s ws[n])),
           added to S[n].
  output   one rounding to the INT16 step at the site's calibrated output
           range: step = max|y| / 32767, codes clamp(rne(y / step),
           -32768, 32767).

Other sites keep the deployed per-row arithmetic. Result files go to --out,
never into the mimus checkout. Usage:

  MIMUS/.venv/bin/python tools/npu-mimus-accuracy.py --mimus MIMUS --out DIR \
      eval --stats STATS --set dev-other
  MIMUS/.venv/bin/python tools/npu-mimus-accuracy.py --mimus MIMUS --out DIR report

`report` pairs each set against the fp32 and deployed-arithmetic (control)
results in MIMUS/results with 10,000 paired bootstrap resamples. A set passes
when the 95% interval of the relative WER change against fp32 contains 0 and
the point estimate is at most +1% (mimus docs/int8_activations.md).
"""
import argparse
import json
import os
from pathlib import Path
import sys

QMAX = 32767.0
SETS = ('dev-other', 'dev-clean', 'replay')


def load_mimus(root):
    sys.path.insert(0, str(root / 'train'))
    import static_scales  # pylint: disable=import-outside-toplevel
    return static_scales


def install(static_scales, out):
    """Routes int8-weight 16-bit sites with an `npu` record through the NPU model."""
    import torch  # pylint: disable=import-outside-toplevel
    quant = static_scales.quant
    deployed = quant.QuantSite.matmul

    def npu_matmul(self, x2d):
        p = getattr(self, 'npu', None)
        if p is None:
            return deployed(self, x2d)
        s = p['input_t'] / QMAX
        x = torch.clamp(torch.round(x2d.double() / s), -QMAX, QMAX)
        codes, ws = quant.quantize_weight_codes(self.weight2d(), self.cfg, self.scales)
        unit = s * ws.double().flatten()
        acc = torch.nn.functional.linear(x, codes.double())  # exact: |S| < 2^53
        if self.bias is not None:
            acc = acc + torch.round(self.bias.double() / unit)
        step = p['output_t'] / QMAX
        y = torch.clamp(torch.round(acc * unit / step), -QMAX - 1, QMAX) * step
        return y.to(x2d.dtype)

    quant.QuantSite.matmul = npu_matmul

    def configure(model, args):
        stats = static_scales.torch.load(args.stats, weights_only=False)['sites']
        key = 'amax' if args.calib == 'max' else f'abs_p{args.calib[1:]}'
        chosen = []
        for name, site in static_scales.int16_sites(model).items():
            if site.cfg.weight != 'int8' or name not in stats:
                continue
            site.npu = {'input_t': stats[name]['in'][key], 'output_t': stats[name]['out']['amax']}
            chosen.append(name)
        return {'npu_int16_sites': len(chosen), 'input_calib': args.calib,
                'output_range': 'calibrated max |y|', 'bias': 'int32 in units of s ws[n]',
                'stats': args.stats}

    static_scales.configure = configure
    static_scales.RESULTS = str(out)


def evaluate(static_scales, args):
    install(static_scales, args.out)
    eval_args = argparse.Namespace(sim=args.sim or static_scales.SIM, checkpoint=static_scales.CHECKPOINT,
                                   device=args.device, stats=args.stats, calib=args.calib, set=args.set,
                                   name=args.name, perturb='', limit=args.limit)
    static_scales.evaluate(eval_args)


def report(static_scales, args):
    from compare_wer import compare  # pylint: disable=import-outside-toplevel
    results = Path(static_scales.RESULTS)
    rows = {}
    for name in SETS:
        variant = args.out / f'{args.name}_{name}.utts.jsonl'
        if not variant.exists():
            continue
        vs_fp32 = compare(str(results / f'fp32_{name}.utts.jsonl'), str(variant), args.samples)
        vs_control = compare(str(results / f'static_control_{name}.utts.jsonl'), str(variant), args.samples)
        lo, hi = vs_fp32['rel_change_95ci_pct']
        rows[name] = dict(vs_fp32=vs_fp32, vs_control=vs_control,
                          passes=bool(lo <= 0 <= hi and vs_fp32['rel_change_pct'] <= 1.0),
                          run=json.loads((args.out / f'{args.name}_{name}.json').read_text()))
    report = dict(variant=args.name, samples=args.samples, sets=rows,
                  passes=bool(rows) and all(r['passes'] for r in rows.values()) and len(rows) == len(SETS))
    (args.out / f'{args.name}.report.json').write_text(json.dumps(report, indent=1) + '\n')
    for name, row in rows.items():
        f, c = row['vs_fp32'], row['vs_control']
        print(f"{name}: WER {f['system_wer']} vs fp32 {f['baseline_wer']}: {f['rel_change_pct']:+.2f}% "
              f"[{f['rel_change_95ci_pct'][0]:+.2f}, {f['rel_change_95ci_pct'][1]:+.2f}]; vs control "
              f"{c['baseline_wer']}: {c['rel_change_pct']:+.2f}% [{c['rel_change_95ci_pct'][0]:+.2f}, "
              f"{c['rel_change_95ci_pct'][1]:+.2f}]; passes={row['passes']}")
    print('all sets pass' if report['passes'] else 'not every set passes')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--mimus', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--name', default='npu_int16')
    sub = ap.add_subparsers(dest='command', required=True)
    e = sub.add_parser('eval')
    e.add_argument('--stats', required=True)
    e.add_argument('--calib', default='p99.999', help='max or p<percentile> of the calibrated |x|')
    e.add_argument('--set', nargs='+', default=list(SETS))
    e.add_argument('--sim', default='')
    e.add_argument('--device', default='cuda')
    e.add_argument('--limit', type=int, default=0)
    r = sub.add_parser('report')
    r.add_argument('--samples', type=int, default=10000)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    root = args.mimus.resolve()
    os.chdir(root)
    static_scales = load_mimus(root)
    {'eval': evaluate, 'report': report}[args.command](static_scales, args)


if __name__ == '__main__':
    main()
