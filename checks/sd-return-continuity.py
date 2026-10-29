#!/usr/bin/env python3
"""Actual SD producer/decoder CLI with modeled warm continuity; no USB."""
import argparse
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--checkpoint', type=Path, required=True)
    p.add_argument('--native', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args(); a.output.mkdir(parents=True, exist_ok=False)
    spec = importlib.util.spec_from_file_location('fixture', ROOT/'checks/storage-read-returning-host.py')
    fixture = importlib.util.module_from_spec(spec); spec.loader.exec_module(fixture)
    tool = fixture.tool; calls = []
    def write(path, value, stamp=None):
        path.write_text(json.dumps(value))
        if stamp is not None: os.utime(path, (stamp, stamp))
    def cli(name, args, refusal=None):
        command = [sys.executable, '-B', str(ROOT/'tools/storage-read-returning.py'), *map(str, args)]
        result = subprocess.run(command, capture_output=True, text=True)
        calls.append(dict(name=name, command=command, exit=result.returncode,
                          stdout=result.stdout, stderr=result.stderr))
        (a.output/'calls.json').write_text(json.dumps(calls, indent=2)+'\n')
        assert (result.returncode == 0) if refusal is None else (result.returncode != 0 and refusal in result.stderr), calls[-1]
    ddr, survey = fixture.lifetime(a.output, 'model')
    past = time.time()-50
    d = json.loads((ddr/'session.json').read_text()); d['tool_sha256'] = '4a54cdc179baafbdecac47eb85714c3745d6d9952e05b482c7016afd4b22bf3e'
    write(ddr/'session.json', d, past)
    previous_node, previous_identity = d['device'], d['node_identity']; links = []
    for index in range(2):
        link = copy.deepcopy(json.loads(a.checkpoint.read_text()))
        link.update(initial_node=previous_node, initial_identity=previous_identity,
                    returned_node=f'/dev/bus/usb/003/{901+index}', returned_identity=[7, 9000+index, 8000+index, 0, 0],
                    model='MODELED CONTINUITY AND AUTHORITY; NOT PHYSICAL')
        path = a.output/f'link-{index}.json'; write(path, link, past+index+1); links.append(path)
        previous_node, previous_identity = link['returned_node'], link['returned_identity']
    s = json.loads((survey/'session.json').read_text())
    s.update(node=previous_node, identity=previous_identity, physical_access=True, tool_sha256=d['tool_sha256'])
    write(survey/'session.json', s, past+10)
    fabric = a.output/'fabric'; fabric.mkdir()
    f = dict(schema=tool.READ_SCHEMA, complete=True, scratch_restored=True, port='3-3',
             node=previous_node, identity=previous_identity,
             words={'0x02002500': 0x03000002, '0x02002580': 0xc3000000, '0x02002588': 0x84000000},
             model='MODELED FABRIC; NOT PHYSICAL')
    write(fabric/'session.json', f, past+11)
    config = a.output/'config.json'; write(config, {k:v for k,v in fixture.CONFIG.items() if not k.startswith('fabric')})
    request = a.output/'request.json'
    write(request, {'fields': {'A': {'initial':['authorize_one_cmd0_initialization'], 'recovery':sorted(tool.LEGACY_RECOVERY)}}})
    approval = fixture.approval(a.output/'approval.json', 'A',
        {'initial': {'authorize_one_cmd0_initialization':True}, 'recovery':dict.fromkeys(tool.RESTORED_RECOVERY, True)})
    common = ['--approval', approval, '--ddr', ddr, '--survey', survey, '--request', request,
              '--return-policy', tool.CHAIN_RETURN]
    warm = [v for path in links for v in ('--warm-return', path)]
    cli('ordered sign', ['sign', *common, '--signer', 'MODEL', *warm])
    inp = a.output/'input.bin'
    produce = ['input', *common, '--fabric-read', fabric, '--config', config, '--profile', '2',
               '--plan', 'chain-read', '--output', inp]
    cli('ordered input', [*produce, *warm])
    raw_input = inp.read_bytes()
    provenance = json.loads(inp.with_suffix('.json').read_text())
    assert provenance['warm_return_sha256'] == [tool.digest(path) for path in links]
    assert provenance['effective_identity'] == previous_identity
    inp.unlink()
    cli('missing chain', produce, 'another node or ROM identity')
    cli('missing first link', [*produce, '--warm-return', links[1]], 'contiguous successful')
    cli('reordered chain', [*produce, '--warm-return', links[1], '--warm-return', links[0]], 'contiguous successful')
    original = links[0].read_bytes(); original_time = links[0].stat().st_mtime
    for key, replacement, reason in [('dirty',True,'not a quiescent'), ('dram_restored',False,'not a quiescent'),
                                      ('ownership_release_permitted',False,'not a quiescent'),
                                      ('transport_failed',True,'contiguous successful')]:
        bad = json.loads(original); bad[key] = replacement; write(links[0], bad, original_time)
        cli(key, [*produce, *warm], reason)
    for code in (8,9,10):
        bad = json.loads(original); bad['npu_result']['mbus']['restore_result'] = code
        write(links[0], bad, original_time); cli(f'busy MBUS {code}', [*produce, *warm], 'not a quiescent')
    bad = json.loads(original); bad['npu_result'].pop('mbus')
    write(links[0], bad, original_time); cli('missing MBUS', [*produce, *warm], 'not a quiescent')
    bad = json.loads(original); bad['unrelated_but_hashed'] = True
    write(links[0], bad, original_time); cli('tampered chain hash', [*produce, *warm], 'another warm-return chain')
    links[0].write_bytes(original); os.utime(links[0], (original_time, original_time))
    write(survey/'session.json', s, past); cli('stale survey', [*produce, *warm], 'survey is older')
    write(survey/'session.json', s, past+10)
    write(fabric/'session.json', f, past); cli('stale fabric', [*produce, *warm], 'fabric read is older')
    write(fabric/'session.json', f, past+11)
    # Use the accepted linked producer's chain output; only model its startup
    # generation line for the fresh input, as the native fixture starts below main.
    inp.write_bytes(raw_input); write(inp.with_suffix('.json'), provenance)
    generation = tool.parse_input(raw_input)['generation']
    log = (a.native/'chain.log').read_text().replace('SD_GENERATION=0x0000000001234567',
                                                 f'SD_GENERATION=0x{generation:016x}')
    run = a.output/'run'; run.mkdir(); (run/'kernel.log').write_text(log)
    session = dict(complete=True, launch_attempted=True, transport_failed=False, automatic_access=True,
        mode='kernel', initial_node=previous_node, initial_identity=previous_identity,
        returned_node='/dev/bus/usb/003/903', result={'magic':0x534b4552,'stage':5,'status':0},
        admission=dict(ddr_sha256=provenance['ddr_session_sha256'], survey_sha256=provenance['survey_session_sha256'],
                       warm_returns=[{'sha256':h} for h in provenance['warm_return_sha256']]))
    write(run/'session.json', session)
    reference = a.native.parent/'host/chain-reference.json'
    decode = ['decode','--run',run,'--input',inp,'--reference',reference,'--require-restored-return',
              '--output',a.output/'decoded.json']
    cli('strict warm capture', decode)
    for name, change in [('capture missing chain', []), ('capture reordered chain', session['admission']['warm_returns'][::-1])]:
        bad = copy.deepcopy(session); bad['admission']['warm_returns'] = change; write(run/'session.json', bad)
        cli(name, decode, 'capture differs from signed warm-return chain')
    write(run/'session.json', session)
    bad = dict(provenance); bad.pop('warm_return_sha256'); write(inp.with_suffix('.json'), bad)
    cli('sidecar missing chain', decode, 'capture differs from signed warm-return chain')
    write(inp.with_suffix('.json'), provenance)
    bad = copy.deepcopy(session); bad['initial_identity'][1] += 1; write(run/'session.json', bad)
    cli('wrong effective identity', decode, 'effective warm-return identity')
    (a.output/'result.json').write_text(json.dumps(dict(passed=True, physical=False, cli_cases=len(calls),
        checkpoint_source=str(a.checkpoint), checkpoint_sha256=hashlib.sha256(a.checkpoint.read_bytes()).hexdigest(),
        native_reused=str(a.native), scope='Modeled authority/continuity; actual CLI and accepted native output; no native replay'), indent=2)+'\n')


if __name__ == '__main__': main()
