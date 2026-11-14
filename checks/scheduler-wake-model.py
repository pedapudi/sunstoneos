#!/usr/bin/env python3
"""Exhaust bounded wake protocols and require shortest negative traces."""
import argparse
import importlib.util
import itertools
import json
from pathlib import Path
import resource
import sys
import time
GATE_RUNS = [['--build-dir', '{output}']]

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('wake_explore', ROOT/'kernel/scheduler/model/explore.py')
model = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = model
spec.loader.exec_module(model)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    args.build_dir.mkdir(parents=True, exist_ok=True)
    results = []
    def run(name, factory, negative=False):
        start = time.monotonic()
        states, edges, trace = model.explore(*factory)
        if (trace is not None) != negative:
            raise AssertionError(f'{name}: unexpected counterexample={trace}')
        row = dict(name=name, states=states, edges=edges,
                   seconds=round(time.monotonic()-start, 4), negative=negative,
                   counterexample=trace)
        results.append(row)
        print(json.dumps(row), flush=True)
    for actions in itertools.product(('give','bits','overwrite','nooverwrite','clear'), repeat=2):
        run('notification/'+','.join(actions), model.notification(actions))
    run('negative/sticky-latch', model.notification(('give','clear'),sticky=False),True)
    run('negative/count-before-ticket',model.notification(('give','clear'),register_first=False),True)
    run('negative/transient-observer',model.notification(('give','clear'),observe_gate=False),True)
    run('block/PREPARING-timeout-switch',model.block_race())
    run('negative/timeout-winner',model.block_race(winner_check=False),True)
    run('retirement',model.retirement())
    run('negative/pin-DMB',model.retirement(pin_barrier=False),True)
    run('negative/close-DMB',model.retirement(close_barrier=False),True)
    run('negative/detached-drain',model.retirement(drain=False),True)
    run('migration',model.migration())
    run('negative/forwarding',model.migration(forward=False),True)
    run('negative/forwarding-retirement',model.migration(retain=False),True)
    run('negative/priority-republish',model.migration(republish=False),True)
    run('mailbox/outbox-and-spill', model.mailbox())
    run('mailbox/three-hints', model.mailbox(hints=3))
    run('negative/mailbox-no-SGI', model.mailbox(signal=False), True)
    run('negative/consumed-before-read', model.mailbox(release_after_read=False), True)
    run('negative/spill-count-before-bit', model.mailbox(bit_before_count=False), True)
    run('retirement/last-put-close', model.last_put_close())
    run('negative/last-put-DMB', model.last_put_close(put_barrier=False), True)
    run('migration/publisher-drain-ack', model.migration_ack())
    run('negative/early-old-owner-ack', model.migration_ack(ack_after_publishers=False), True)
    run('migration/routing-epoch-admission', model.route_epoch())
    run('negative/route-epoch-validation', model.route_epoch(validate=False), True)
    report=dict(results=results,peak_rss_kib=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
                child_peak_rss_kib=resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)
    (args.build_dir/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k!='results'}))


if __name__ == '__main__':
    main()
