#!/usr/bin/env python3
"""Compare linked 5.4 Gb/s DisplayPort programming with pinned vendor functions.

PHY/sink effects, native admission and elapsed time are modeled. This checks
register traces and protocol choices; it does not qualify electrical output.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess

import harness

ROOT = Path(__file__).resolve().parents[1]
PHY = 'bsp/drivers/phy/sunxi-cadence-combophy.c'
TR = 'bsp/drivers/drm/sunxi_device/hardware/lowlevel_edp/trilinear_dp14/trilinear_dp14.c'
EDP = 'bsp/drivers/drm/sunxi_device/sunxi_edp.h'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def function(source, name):
    match = re.search(r'^(?:static )?(?:int|void|bool|s32|u32|u64) '+name+r'\([^;]+?\)\s*\{', source, re.M)
    assert match, name
    body = source.index('{', match.start())
    end, depth = body+1, 1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end += 1
    return source[match.start():end]+'\n'


PHY_MODEL = r'''
#include <stdio.h>
#include <stdlib.h>
static uint32_t registers[0x100000/4];
static unsigned comma;
static void separator(void){if(comma++)printf(",");}
int a733_dp_read(struct a733_dp *d,uintptr_t a,unsigned n,uint32_t *v)
{
 (void)d;separator();printf("[\"read\",%lu,%u]",(unsigned long)a,n);
 if(a==0x06c01900)*v=0x111;
 else {*v=registers[(a-0x06c00000)/4];if(n==2)*v=(*v>>((a&2)*8))&65535;}
 return 1;
}
int a733_dp_write(struct a733_dp *d,uintptr_t a,unsigned n,uint32_t v)
{
 (void)d;separator();printf("[\"write\",%lu,%u,%u]",(unsigned long)a,n,v);
 uint32_t *p=&registers[(a-0x06c00000)/4];
 if(n==2){unsigned shift=(a&2)*8;*p=(*p&~(65535u<<shift))|((v&65535)<<shift);}else *p=v;
 return 1;
}
int a733_dp_wait(struct a733_dp *d,unsigned us)
{(void)d;separator();printf("[\"wait\",%u]",us);return 1;}
int a733_dp_fault(struct a733_dp *d,enum a733_dp_error e)
{(void)d;(void)e;abort();}
int main(int argc,char **argv)
{
 if(argc!=4)return 2;
 struct a733_dp d={0};struct sunxi_cadence_phy p={.top_combo_reg=0x06c06000};
 struct sunxi_cadence_combophy c={.dp=&d,.sunxi_cphy=&p,.top_reg=0x06c01000,
  .phy_reg=0x06c80000,.orientation=(unsigned)atoi(argv[1]),.link_rate=5400};
 for(unsigned i=0;i<4;i++){c.lane_remap[i]=i;c.typec_remap[i]=i;c.lane_invert[i]=1;}
 int swing=atoi(argv[2]),pre=atoi(argv[3]);
 struct phy_configure_opts_dp o={.lanes=4,.link_rate=5400};
 printf("[");
 if(swing<0){o.set_lanes=o.set_rate=1;if(combo0_dp_set_lanes(&c,&o)||combo0_dp_set_rate(&c,&o))return 3;}
 else {o.set_voltages=1;for(unsigned i=0;i<4;i++){o.voltage[i]=(unsigned)swing;o.pre[i]=(unsigned)pre;}
  if(combo0_dp_set_sw_pre(&c,&o))return 4;}
 printf("]\n");return 0;
}
'''

TRANSPORT_MODEL = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
typedef uint32_t u32;typedef uint64_t u64;typedef int32_t s32;
#define RET_OK 0
#define LS_PER_TU 64
struct sunxi_edp_hw_desc {unsigned unused;};
struct edp_tx_core {struct {u32 pixel_clk;} timings;struct {u64 bit_rate;} lane_para;};
static u32 registers[0x1000/4];
#define TR_READ(hw,r) ((void)(hw),registers[(r)/4])
#define TR_WRITE(hw,r,v) ((void)(hw),registers[(r)/4]=(v))
#define TR_SET_BITS(hw,r,s,w,v) ((void)(hw),registers[(r)/4]=(registers[(r)/4]&~((UINT32_MAX>>(32-(w)))<<(s)))|((u32)(v)<<(s)))
'''


def admission_cases(preparer, image_hash):
    """Exercise the real fact loop; unchanged physical/board/allocation gates are modeled.

    Every input is marked modeled=True. The production physical-authority
    function would reject these facts. Nothing here produces launch authority.
    """
    fields={
        'isolation':('all_masters_excluded','prior_transactions_drained','prior_walks_drained',
                     'remote_secure_writers_excluded','display_irqs_owned_masked','iommu_irqs_owned_masked'),
        'memory':('private_nc_arena','no_physical_aliases','iommu_banks_disabled','iommu_clocks_stable','display_device_mapping'),
        'analog':('dp_analog_qualified','source_apb_independently_qualified'),
        'power':('typec24_source_limits_qualified','dp_supplies_stable'),
        'serdes':('comb0_exclusive_from_usb3','four_lane_dp_route_qualified','source_clocks_stable','source_apb_independently_qualified'),
        'supervision':('whole_board_power_removal','permanent_dma_retention','no_reuse_after_fault','one_attempt')}
    original_physical=preparer['physical_authority']
    original_board=preparer['board'].validate_admission
    original_allocation=preparer['allocation_check']
    original_rate=preparer['link_rate_authority']
    visited=[]

    def modeled(fact):
        assert fact['modeled'] is True

    def rate(fact):
        visited.append(fact['schema'])
        original_rate(fact)

    def fixture(changed=None, value=5400):
        raw=bytearray(952)
        struct.pack_into('<I',raw,860,200000000)
        cache={}
        record={'schema':'a733-displayport-supervised-entry-v1','modeled':True,
                'identity':'model-only','port':'model-only'}

        def evidence(name, data):
            path='/model-only/'+name
            cache[path]=data
            return {'path':path,'sha256':hashlib.sha256(data).hexdigest()}

        apb=evidence('apb',b'modeled APB observation')
        for index,(name,flags) in enumerate(fields.items()):
            fact=dict(schema='a733-displayport-'+name+'-v1',modeled=True,reviewed=True,
                      identity=record['identity'],port=record['port'],image_sha256=image_hash,
                      **{flag:True for flag in flags})
            if name in ('analog','serdes'):
                fact.update(qualified_link_rate_mbps=5400,qualified_apb_hz=200000000,apb_evidence=[apb])
                if name==changed:
                    if value is None:fact.pop('qualified_link_rate_mbps')
                    else:fact['qualified_link_rate_mbps']=value
            data=json.dumps(fact,sort_keys=True).encode()
            record[name]=evidence(name,data)
            raw[640+index*32:672+index*32]=hashlib.sha256(data).digest()
        template_hash=hashlib.sha256(raw).hexdigest()
        ddr=dict(schema='a733-ddr-reservation-v1',modeled=True,identity=record['identity'],
                 port=record['port'],image_sha256=image_hash,exclusive=True,published_cache_disabled=True,
                 remote_writers_excluded=True,ranges=[[0x48000000,0x79000000]],evidence=[apb])
        record['displayport_ddr']=evidence('ddr',json.dumps(ddr).encode())
        allocation=dict(schema='a733-displayport-allocation-v1',modeled=True,
                        image_sha256=image_hash,session_sha256=template_hash)
        record['allocation']=evidence('allocation',json.dumps(allocation).encode())
        record['page_table']=evidence('table',b'modeled table')
        record['allocation_bytes']=evidence('bytes',b'modeled allocation')
        return record,raw,template_hash,cache

    preparer['physical_authority']=modeled
    preparer['board'].validate_admission=lambda common,image,template,cache,frozen:cache
    preparer['allocation_check']=lambda *unused:None
    preparer['link_rate_authority']=rate
    cases=[]
    try:
        for frozen in (False,True):
            record,raw,template_hash,cache=fixture()
            visited.clear()
            assert preparer['validate_admission'](record,image_hash,template_hash,raw,cache,frozen)==cache
            assert visited==['a733-displayport-analog-v1','a733-displayport-serdes-v1']
            cases.append('both-rate-facts-accepted-'+str(frozen))
            for name in ('analog','serdes'):
                for value in (None,1620,2700,5400.0):
                    record,raw,template_hash,cache=fixture(name,value)
                    harness.refused(lambda:preparer['validate_admission'](
                        record,image_hash,template_hash,raw,cache,frozen),'5.4 Gb/s')
                    cases.append(name+'-rate-'+str(value)+'-refused-'+str(frozen))
    finally:
        preparer['physical_authority']=original_physical
        preparer['board'].validate_admission=original_board
        preparer['allocation_check']=original_allocation
        preparer['link_rate_authority']=original_rate
    return cases


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('image', 'previous-image', 'reference', 'output', 'preparer'):
        parser.add_argument('--'+name, type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    commands = []

    def run(command):
        result = subprocess.run(list(map(str, command)), cwd=ROOT, env=harness.environment(),
                                capture_output=True, text=True)
        commands.append(dict(command=list(map(str, command)), returncode=result.returncode,
                             stdout=result.stdout, stderr=result.stderr))
        (args.output/'commands.json').write_text(json.dumps(commands, indent=2)+'\n')
        result.check_returncode()
        return result.stdout

    manifest = json.loads((ROOT/'apps/displayport/sources.json').read_text())
    sources = {}
    identities = {}
    for name in (PHY, TR, EDP):
        data = (args.reference/name).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        assert digest == manifest['reference_pins'][name]['sha256'], name
        sources[name] = data.decode()
        identities[name] = digest
    native = (ROOT/'drivers/display/a733_dp_cadence.c').read_text()
    preamble = native[:native.index('/* Sub-System PCIE Bus Gating Reset Register */')]
    vendor = sources[PHY]
    constants = vendor[vendor.index('/* Sub-System PCIE Bus Gating Reset Register */'):
                       vendor.index('static void sunxi_cadence_phy_reset_control_put')]
    functions = ''.join(function(vendor, name) for name in
                        ('combo0_dp_phy_reset', 'combo0_dp_set_rate', 'combo0_dp_set_lanes',
                         'combo0_dp_set_sw_pre', 'combo0_configure_usb_dp'))
    reference_c = args.output/'phy-reference.c'
    reference_c.write_text(vendor[:vendor.index('#include')]+preamble+constants+
                           '\nstatic int combo0_configure_usb_dp(struct sunxi_cadence_combophy *);\n'+functions+PHY_MODEL)
    binary = args.output/'phy-reference'
    flags = [str(harness.CLANG), '-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-parameter', '-Wno-unused-but-set-variable', '-Wno-sign-compare', *harness.SANITIZE,
             '-I'+str(ROOT/'include'), '-I'+str(ROOT/'drivers/display')]
    run([*flags, reference_c, '-o', binary])

    source = sources[TR]
    header = args.reference/'bsp/drivers/drm/sunxi_device/hardware/lowlevel_edp/trilinear_dp14/trilinear_dp14.h'
    data = header.read_bytes()
    assert hashlib.sha256(data).hexdigest()==manifest['reference_pins'][str(header.relative_to(args.reference))]['sha256']
    identities[str(header.relative_to(args.reference))] = hashlib.sha256(data).hexdigest()
    rate_define = re.search(r'^#define BIT_RATE_5G4 .*$', sources[EDP], re.M)[0]
    transport_c = args.output/'transport-reference.c'
    transport_c.write_text(source[:source.index('#include')]+TRANSPORT_MODEL+'\n'+
        '#include "'+str(header)+'"\n'+rate_define+'\n'+''.join(function(source, name) for name in
        ('trilinear_get_max_rate', 'trilinear_transfer_unit_config', 'trilinear_set_video_timestamp',
         'edp_set_secondary_data_window', 'trilinear_support_tps3'))+r'''
int main(void){struct sunxi_edp_hw_desc hw={0};struct edp_tx_core c={0};
 c.timings.pixel_clk=74250000;c.lane_para.bit_rate=trilinear_get_max_rate(&hw);
 trilinear_set_video_timestamp(&hw,&c);registers[TR_SRC0_MAIN_STREAM_HSTART/4]=260;
 edp_set_secondary_data_window(&hw,c.lane_para.bit_rate,74250);
 if(trilinear_transfer_unit_config(&hw,24,4,c.lane_para.bit_rate,74250))return 1;
 printf("[%llu,%u,%u,%u,%u,%u]\n",(unsigned long long)c.lane_para.bit_rate,
 registers[TR_SRC0_MVID/4],registers[TR_SRC0_NVID/4],registers[TR_SRC0_TU_CONFIG/4],
 registers[TR_SRC0_SECOND_DATA_WINDOW/4],trilinear_support_tps3(&hw));return 0;}
''')
    transport_binary = args.output/'transport-reference'
    run([*flags, transport_c, '-o', transport_binary])
    transport = json.loads(run([transport_binary]))
    assert transport == [5400000000, 74250, 540000, 0x09060040, 945, 1]

    controller = load('dp_top_rate_controller', ROOT/'checks/displayport-controller.py')
    image = controller.base.Image(args.image, layout_names=('a733_dp','a733_dp_ops','a733_dp_config','a733_dp_result'))

    class Model(controller.Model):
        def __init__(self, image):
            super().__init__(image)
            self.aux_writes = []
            self.pattern_order = []

        def callback(self, u, at, size, unused):
            if (at-self.callback_base)//16==5:
                self.events.append(['wait', u.reg_read(controller.UC_ARM64_REG_X0+1)])
            super().callback(u, at, size, unused)

        def complete(self):
            pending = self.pending
            if pending and self.time>=pending[0] and self.mode!='timeout':
                _, command, address, data = pending
                if ((command>>8)&15)==8:
                    self.aux_writes.append((address, list(data)))
                    if address==0x102:self.pattern_order.append(('receiver', data[0]&7))
            if self.pattern==3 and pending and ((pending[1]>>8)&15)==9 and pending[2]==0x202:
                # The reviewed lower model handles EQ status for pattern 2.
                # Pattern 3 has the same modeled CR/EQ/alignment observations.
                self.pattern=2
                super().complete()
                self.pattern=3
            else:
                super().complete()

        def write(self, address, width, value):
            if address==0x0574000c:self.pattern_order.append(('source', value&7))
            return super().write(address, width, value)

    counts = []
    for polarity in (1,2):
        model=Model(image)
        model.put(model.cfg+image.layouts['a733_dp_config']['polarity'], polarity, 4)
        assert model.call('a733_dp_init', model.obj, model.cfg)==1
        model.events.clear()
        assert model.call('a733_dp_cadence_prepare', model.obj)==1
        events=model.events
        # Unchanged source lane remap/inversion precedes rate configuration;
        # unchanged AUX power-on follows the distinct 10 ms wait.
        assert events[0]==['write',0x06c06c24,4,0x3210]
        finish=events.index(['wait',10000])
        rate_events=events[9:finish]
        expected=json.loads(run([binary, polarity, -1, 0]))
        assert rate_events==expected, ('PHY rate trace',polarity)
        counts.append(dict(polarity=polarity, phase='rate', operations=len(rate_events)))
        for swing in range(4):
            for pre in range(4-swing):
                model.events.clear()
                training=bytes([swing|(pre<<3)]*4)
                address=0x43e02000
                model.u.mem_write(address, training)
                assert model.call('a733_dp_cadence_levels', model.obj, address)==1
                expected=json.loads(run([binary, polarity, swing, pre]))
                assert model.events==expected, ('PHY levels',polarity,swing,pre)
                counts.append(dict(polarity=polarity,phase='levels',swing=swing,pre=pre,operations=len(expected)))
    cases=[]
    for polarity in (1,2):
        for tps3 in (False,True):
            model=Model(image)
            model.put(model.cfg+image.layouts['a733_dp_config']['polarity'], polarity, 4)
            model.dpcd[2] |= 0x40 if tps3 else 0
            assert model.call('a733_dp_init',model.obj,model.cfg)==1
            assert model.call('a733_dp_prepare',model.obj)==1
            assert (0x100,[20,0x84]) in model.aux_writes
            selected=3 if tps3 else 2
            assert model.pattern_order==[('receiver',1),('source',1),('receiver',selected),
                                        ('source',selected),('receiver',0),('source',0)]
            expected={0x05740000:20,0x0574084c:transport[1],0x05740854:transport[2],
                      0x05740850:transport[3],0x05740808:transport[4]}
            assert all(model.registers[address]==value for address,value in expected.items())
            prepare_operations=len(model.events)
            assert model.call('a733_dp_start',model.obj)==1
            assert model.call('a733_dp_service',model.obj)==1
            assert model.call('a733_dp_stop',model.obj)==1
            cases.append(dict(polarity=polarity,tps3=tps3,prepare_operations=prepare_operations,operations=len(model.events),
                              cr_attempts=model.cr,eq_attempts=model.eq))
    for rate in (6,10):
        model=Model(image);model.dpcd[1]=rate
        assert model.call('a733_dp_init',model.obj,model.cfg)==1
        assert model.call('a733_dp_prepare',model.obj)==0
        assert struct.unpack_from('<I',model.result(),4)[0]==7 # A733_DP_SINK
        assert 0x05740080 not in model.registers and 0x05740000 not in model.registers
        assert not any(address==0x100 for address,data in model.aux_writes)
    old_image=controller.base.Image(args.previous_image,layout_names=('a733_dp','a733_dp_ops','a733_dp_config','a733_dp_result'))
    old=Model(old_image)
    assert old.call('a733_dp_init',old.obj,old.cfg)==1 and old.call('a733_dp_prepare',old.obj)==1
    before_count=len(old.events)

    # Execute the proposed shared preparer with its real source location, so
    # its imports and actual ELF immutability checks use this checkout.
    preparer={'__file__':str(ROOT/'apps/displayport/prepare.py'),'__name__':'dp_rate_preparation'}
    exec(compile(args.preparer.read_text(),str(args.preparer),'exec'),preparer)
    preparer['image_parts'](args.image)
    harness.refused(lambda:preparer['image_parts'](args.previous_image),'displayport_link_rate_mbps')
    for value in (None,1620,2700,True,5400.0):
        harness.refused(lambda:preparer['link_rate_authority']({'qualified_link_rate_mbps':value}),
                        '5.4 Gb/s')
    preparer['link_rate_authority']({'qualified_link_rate_mbps':5400})
    authority_cases=admission_cases(preparer,hashlib.sha256(args.image.read_bytes()).hexdigest())

    inputs=[Path(__file__), ROOT/'checks/displayport-controller.py',ROOT/'checks/harness.py',
            ROOT/'drivers/display/a733_dp.c',ROOT/'drivers/display/a733_dp_cadence.c',
            ROOT/'drivers/display/a733_dp_internal.h',ROOT/'apps/displayport/admission.c',
            args.image,args.previous_image,args.preparer]
    report=dict(passed=True,physical_access=False,scope=__doc__,references=identities,
                transport=transport,phy_cases=counts,controller_cases=cases,authority_cases=authority_cases,
                previous_prepare_operations=before_count,
                current_prepare_operations=cases[0]['prepare_operations'],
                inputs={str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs})
    (args.output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('PASS vendor PHY traces for both polarities and ten legal signal levels; transport, TPS2/TPS3, sink and authority refusals')


if __name__=='__main__':
    main()
