#!/usr/bin/env python3
"""Check only changed DE_SYS host predicates using the selected model fixture.

Unchanged board authority is substituted, and validation stops before DDR and
allocation admission. These cases establish no full physical admission, launch,
capture, startup or native execution. Mutated evidence is resealed by the actual
G2D producer so a digest mismatch cannot masquerade as the intended refusal.
"""
import argparse,copy,hashlib,importlib.util,io,json,struct
from pathlib import Path
from unittest.mock import patch
from elftools.elf.elffile import ELFFile
ROOT=Path(__file__).resolve().parents[1]
def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
p=argparse.ArgumentParser(description=__doc__)
for name in ('image','fixture','output'):p.add_argument('--'+name,type=Path,required=True)
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
producer=module('g2d_power_admission_producer',ROOT/'apps/g2d/producer.py');prep=producer.preparation()
image=a.image.read_bytes();session=(a.fixture/'session.bin').read_bytes();source=(a.fixture/'input.argb').read_bytes()
facts={key:(a.fixture/(key+'.json')).read_bytes() for key in ('initial','memory','electrical','supervision')}
ih=hashlib.sha256(image).hexdigest();prep.embedded_reference(image,session)
class ReachedUnchangedScope(Exception):pass
class Record(dict):
    def __getitem__(self,key):
        if key=='g2d_ddr':raise ReachedUnchangedScope()
        return super().__getitem__(key)
def modeled(fact):assert fact['physical_authority'] is False and fact['modeled'] is True
results=[]
for case in ('nominal','missing-profile','wrong-profile','missing-exclusion','missing-drain'):
    changed=copy.deepcopy(facts);initial=json.loads(changed['initial'])
    if case=='missing-profile':initial.pop('de_sys_power_profile')
    if case=='wrong-profile':initial['de_sys_power_profile']='inherited-on-only'
    if case=='missing-exclusion':initial.pop('de_sys_consumers_excluded')
    if case=='missing-drain':initial.pop('de_sys_prior_transactions_drained')
    changed['initial']=(json.dumps(initial,sort_keys=True,indent=2)+'\n').encode()
    owner,authority,client=struct.unpack_from('<3Q',session,616);g=struct.unpack_from('<12I',session,856)
    raw=producer.extend(session[:592],source,owner=owner,authority=authority,client=client,**changed,
        width=g[0],height=g[1],stride=g[2],operation=g[4],dx=g[5],dy=g[6],w=g[7],h=g[8],sx=g[9],sy=g[10],color=g[11])
    record=Record(schema='a733-g2d-supervised-entry-v1',identity=initial['identity'],port=initial['port'],physical_authority=False,modeled=True)
    cache={}
    for key,data in changed.items():
        path=(a.output/(case+'-'+key+'.json')).resolve();path.write_bytes(data);cache[str(path)]=data
        record[key]={'path':str(path),'sha256':hashlib.sha256(data).hexdigest()}
    (a.output/(case+'-session.bin')).write_bytes(raw)
    try:
        with patch.object(prep,'physical_authority',modeled),patch.object(prep.board,'validate_admission',lambda *args:cache):
            prep.validate_admission(record,ih,hashlib.sha256(raw).hexdigest(),raw)
    except ReachedUnchangedScope:assert case=='nominal';results.append(case)
    except ValueError as error:
        expected='coherent OFF/ON' if case in ('missing-profile','wrong-profile') else 'physical fact absent'
        assert case!='nominal' and expected in str(error),(case,str(error));results.append(case)
    else:raise AssertionError(('changed-scope boundary not reached',case))
elf=ELFFile(io.BytesIO(image));symbols=elf.get_section_by_name('.symtab')
symbol=symbols.get_symbol_by_name('g2d_power_profile')[0];section=elf.get_section(symbol['st_shndx'])
offset=section['sh_offset']+symbol['st_value']-section['sh_addr']
bad=bytearray(image);struct.pack_into('<Q',bad,offset+16,0)
try:prep.embedded_reference(bytes(bad),session)
except ValueError as error:assert 'power profile' in str(error);results.append('wrong-image-marker')
else:raise AssertionError('Wrong power marker accepted')
bad=bytearray(image);strings=elf.get_section(symbols['sh_link']);start=strings['sh_offset']+symbol['st_name'];bad[start]=ord('x')
try:prep.embedded_reference(bytes(bad),session)
except ValueError as error:assert 'marker' in str(error);results.append('missing-image-marker')
else:raise AssertionError('Missing power marker accepted')
report={'passed':True,'cases':results,'image':ih,'session':hashlib.sha256(session).hexdigest(),'physical_authority':False,'scope':__doc__}
(a.output/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({'passed':True,'cases':len(results)}))
