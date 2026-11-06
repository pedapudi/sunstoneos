#!/usr/bin/env python3
"""Check read-only NPU ON admission without opening USB or enabling any gate."""
import argparse
from pathlib import Path
import sys
import types
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--tool-tree', type=Path, default=ROOT)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(args.tool_tree/'tools'))
    import board
    import board_images as images
    good = {0x070101ac: 1, 0x020025c0: 1 << 6, 0x020025e0: 1 << 18,
            0x07064000: 8, 0x07064008: 8}
    variants = [('off', {0x07064000:0, 0x07064008:0}), ('on', {}),
                ('gate-closed', {0x070101ac:0}), ('ahb-closed', {0x020025c0:0}),
                ('mbus-closed', {0x020025e0:0}), ('status-off', {0x07064008:0}),
                ('unknown', {0x07064000:7, 0x07064008:7}),
                ('missing-gate', {0x070101ac:None}), ('missing-status', {0x07064008:None}),
                ('on-high-bits', {0x07064000:0x108, 0x07064008:0x208})]
    kinds = [('finite', {'finite_npu':True, 'symbols':{}}),
             ('generic', {'generic':True, 'symbols':{'ss_application_npu':(0,32)}})]
    for profile, info in kinds:
        for variant, changed in variants:
            words = dict(good)
            words.update(changed)
            evidence = args.out/(profile+'-'+variant)
            evidence.mkdir()
            calls, prepared = [], []
            class Owner(board.Live):
                def __init__(self):
                    super().__init__(types.SimpleNamespace(kind='npu',host_tree=None),evidence,{})
                def tx(self,*operations,**kwargs):
                    assert len(operations)%2==0 and all(operations[i]=='readl' for i in range(0,len(operations),2))
                    addresses=[int(operations[i+1],0) for i in range(0,len(operations),2)]
                    assert all(address in good for address in addresses), addresses
                    if any(address in (0x07064000,0x07064008) for address in addresses):
                        assert words[0x070101ac]&1 and words[0x020025c0]&(1<<6) and words[0x020025e0]&(1<<18)
                    calls.extend(addresses)
                    return ' '.join(hex(words[a]) for a in addresses if words[a] is not None)
                def live_state(self,finite_npu=False):
                    return {'words':{},'port':'3-3','identity':[1,2,3,4,5]}
                def journal(self):pass
                def upload(self,*values,**kwargs):raise AssertionError('admission uploaded an image')
            owner=Owner()
            def prepare(*values,**kwargs):
                prepared.append(values[2]);return {'admitted':True}
            with patch.object(images,'prepare',side_effect=prepare):
                try:owner.prepare(info)
                except ValueError:refused=True
                else:refused=False
            admitted=variant in ('on','on-high-bits')
            assert refused != admitted, (profile,variant,calls,prepared)
            assert bool(prepared)==admitted,(profile,variant)
            if admitted:
                assert calls==list(good),calls
                assert owner.record['npu_on_admission']['status']=='ON'
                assert prepared[0]['words']=={hex(a):words[a] for a in good}
            else:
                assert owner.record['npu_on_admission']['status']=='refused'
                if variant in ('gate-closed','missing-gate'):
                    assert calls==[0x070101ac],calls
                if variant in ('ahb-closed','mbus-closed'):
                    assert not any(a in calls for a in (0x07064000,0x07064008)),calls
            captured={hex(a):v for a,v in words.items() if v is not None}
            try:images.validate_npu_on(captured)
            except ValueError:offline_refused=True
            else:offline_refused=False
            assert offline_refused != admitted,(profile,variant)
            print('PASS',profile,variant)
    # A CPU-only control never reads the PCK controller or PCK4.
    evidence=args.out/'cpu-control';evidence.mkdir()
    owner=board.Live(types.SimpleNamespace(kind='cpu-return'),evidence,{})
    with patch.object(owner,'live_state',return_value={'words':{}}),\
         patch.object(owner,'tx',side_effect=AssertionError('CPU-only image read PCK state')),\
         patch.object(images,'prepare',return_value={'admitted':True}):
        owner.prepare({'symbols':{}})
    print('PASS CPU-only control performs no PCK reads')
    # Missing captured facts refuse the actual offline preparation before it
    # can load an image adapter or create upload/build artifacts.
    for profile,info in kinds:
        evidence=args.out/('offline-missing-'+profile)
        with patch.object(images,'load',side_effect=AssertionError('missing facts reached adapter preparation')):
            try:images.prepare(types.SimpleNamespace(),info,{'words':{}},evidence)
            except ValueError as error:assert 'Missing captured NPU entry gate' in str(error)
            else:raise AssertionError('missing captured state admitted')
        assert not evidence.exists()
        print('PASS offline',profile,'missing state refuses before adapter preparation')


if __name__=='__main__':main()
