"""Only retained board/session/profile identity; hardware boundaries are modeled."""
import hashlib
import importlib.util
import json
import struct
from collections import defaultdict,deque
from pathlib import Path
from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn.arm64_const import UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_PC,UC_ARM64_REG_SP,UC_ARM64_REG_LR

def load(image, kind):
    with image.open('rb') as file:
        elf=ELFFile(file);candidates=defaultdict(set);definitions=defaultdict(set);layouts={}
        for symbol in elf.get_section_by_name('.symtab').iter_symbols():
            if symbol['st_value']:candidates[symbol.name].add(int(symbol['st_value']))
        for cu in elf.get_dwarf_info().iter_CUs():
            unit=cu.get_top_DIE().attributes['DW_AT_name'].value.decode()
            for die in cu.iter_DIEs():
                name=die.attributes.get('DW_AT_name')
                if not name:continue
                name=name.value.decode()
                if die.tag=='DW_TAG_subprogram' and 'DW_AT_low_pc' in die.attributes:
                    definitions[(unit,name)].add(int(die.attributes['DW_AT_low_pc'].value))
                if die.tag=='DW_TAG_structure_type':
                    members={child.attributes['DW_AT_name'].value.decode():int(child.attributes['DW_AT_data_member_location'].value)
                        for child in die.iter_children() if child.tag=='DW_TAG_member' and 'DW_AT_name' in child.attributes and 'DW_AT_data_member_location' in child.attributes}
                    if members:layouts[name]=members
        segments=[(int(s['p_vaddr']),bytes(s.data())) for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
    required=['board_session_prepare','board_session_snapshot','board_identity_prepare','a733_smp_counter','a733_cpu_coherence_publish','storage_digest_bytes','board_radio_chip_id','a733_smp_build_id','a733_board_session_valid','a733_smp_contract_valid']
    if kind=='bluetooth':required+=['bluetooth_admitted_profile','board_application_pins','bluetooth_boot_profile','aarch64_device_translate','board']
    else:required+=['radio_profile','board_radio_profile_runtime','aarch64_device_translate','board','a733_smp_diagnostic_start','a733_smp_device_preparation_admitted','a733_native_usb_init','a733_cached_runtime','platform_panic']
    symbols={}
    for name in required:
        if name=='radio_profile':
            choices={address for (unit,entry),values in definitions.items() if unit.endswith('/apps/board/main.c') and entry==name for address in values}
        else:choices=candidates.get(name,set())
        assert len(choices)==1,(kind,name,choices)
        symbols[name]=next(iter(choices));assert symbols[name] in candidates[name]
    identity=None
    for address,data in segments:
        if address<=symbols['a733_smp_build_id']<=address+len(data)-32:
            identity=data[symbols['a733_smp_build_id']-address:symbols['a733_smp_build_id']-address+32]
    assert identity and len(identity)==32
    return symbols,segments,layouts,identity

def derive_nominal(out):
    """Combine preserved native/component producers using linked field layouts."""
    _,_,layouts,_=load(out/'build/sunstone-a733-bluetooth.elf','bluetooth')
    session=bytearray((out/'native-base-session.bin').read_bytes())
    profile=bytearray((out/'component-profile.bin').read_bytes())
    assert len(session)==592 and len(profile)==624
    board=layouts['a733_board_session'];bt=layouts['bluetooth_profile'];radio=layouts['a733_radio_platform_config']
    for name in ('topology','sid_authority'):
        session[board[name]:board[name]+8]=profile[bt[name]:bt[name]+8]
    for name in ('fabric','fabric_hz'):
        start=bt['radio']+radio[name];profile[start:start+12]=session[board[name]:board[name]+12]
    session[board['sid_evidence']:board['sid_evidence']+32]=hashlib.sha256(profile).digest()
    (out/'bt-session.bin').write_bytes(session);(out/'bt-profile.bin').write_bytes(profile)
    (out/'derived-fixture.json').write_text(json.dumps({'native_base_sha256':hashlib.sha256((out/'native-base-session.bin').read_bytes()).hexdigest(),
        'component_profile_sha256':hashlib.sha256((out/'component-profile.bin').read_bytes()).hexdigest(),
        'adaptations':['Board topology/SID authority from profile','Profile fabric IDs/rates from native board producer','Profile hash bound into session','Per-image linked build ID and produced contract bound by Machine'],
        'dwarf_offsets':{'board':board,'profile':bt,'radio':radio},'session_sha256':hashlib.sha256(session).hexdigest(),
        'profile_sha256':hashlib.sha256(profile).hexdigest(),'authority':'Explicit software model; nondeployable.'},indent=2)+'\n')

class Machine:
    def __init__(self, image, kind, out, source, session, profile, fail_publish=0):
        self.symbols,self.segments,self.layouts,self.identity=load(image,kind)
        self.cpu=Uc(UC_ARCH_ARM64,UC_MODE_ARM);self.cpu.mem_map(0x40000000,0x4000000);self.cpu.mem_map(0x50000000,0x1000000);self.cpu.mem_map(0x03006000,4096)
        for address,data in self.segments:self.cpu.mem_write(address,data)
        self.cpu.mem_write(0x03006200,bytes(range(16)))
        self.publications=[];self.sid=[];self.hashes=[];self.trace=deque(maxlen=32);self.fail_publish=fail_publish;self.finished=False;self.extra=None
        self.raw=bytearray(session);self.raw[48:80]=self.identity
        self.cpu.mem_write(0x41f00000,bytes(self.raw));self.cpu.mem_write(0x43000000,profile)
        spec=importlib.util.spec_from_file_location('seal_board_prepare',source/'apps/board/prepare.py');prepare=importlib.util.module_from_spec(spec);spec.loader.exec_module(prepare)
        prepare.session_check(self.raw)
        epoch,generation,lease=struct.unpack_from('<3Q',self.raw,24)
        _,self.contract=prepare.request_bytes(self.symbols['a733_smp_build_id'],self.identity,epoch,generation,lease,self.raw)
        self.contract=bytearray(self.contract)
        struct.pack_into('<Q',self.contract,0,0x4153373342534332)
        struct.pack_into('<4Q',self.contract,80,24,24000024,1440000024,1440000024)
        self.cpu.mem_write(0x43200000,bytes(self.contract))
        self.cpu.hook_add(UC_HOOK_CODE,self.hook)
        self.cpu.hook_add(UC_HOOK_MEM_READ,self.read)
        assert self.call('a733_board_session_valid',0x41f00000,100)==1,'Nominal native board session invalid'
        assert self.call('a733_smp_contract_valid',0x43200000,100)==1,'Nominal produced native contract invalid'
        if kind=='bluetooth':assert self.call('bluetooth_boot_profile',0x41f00000,0x43000000)==1,'Nominal native Bluetooth boot policy invalid'
    def returned(self,value):
        self.cpu.reg_write(UC_ARM64_REG_X0,value);self.cpu.reg_write(UC_ARM64_REG_PC,self.cpu.reg_read(UC_ARM64_REG_LR))
    def hook(self,cpu,address,size,unused):
        if self.symbols['board_session_prepare']<=address<self.symbols['board_session_prepare']+0x170:
            self.trace.append([address,cpu.reg_read(UC_ARM64_REG_X0)])
        if address==0x50fff000:self.finished=True;cpu.emu_stop();return
        if self.extra and self.extra(address):return
        if address==self.symbols['storage_digest_bytes']:
            pointer=cpu.reg_read(UC_ARM64_REG_X0);count=cpu.reg_read(UC_ARM64_REG_X1)
            self.hashes.append({'address':pointer,'bytes':count,'sha256':hashlib.sha256(cpu.mem_read(pointer,count)).hexdigest()})
        if address==self.symbols['a733_smp_counter']:self.returned(100);return
        elif address==self.symbols['a733_cpu_coherence_publish']:
            self.publications.append((cpu.reg_read(UC_ARM64_REG_X0),cpu.reg_read(UC_ARM64_REG_X1)))
            self.returned(0 if len(self.publications)==self.fail_publish else 1);return
        elif address==self.symbols.get('aarch64_device_translate'):
            target=cpu.reg_read(UC_ARM64_REG_X2);layout=self.layouts['aarch64_translation']
            for name,value in [('physical',cpu.reg_read(UC_ARM64_REG_X0)),('attributes',0xff),('shareability',3)]:
                # Actual structure defines physical64, attributes/shareability32.
                cpu.mem_write(target+layout[name],value.to_bytes(8 if name=='physical' else 4,'little'))
            self.returned(1);return
        # Function boundaries take precedence even when their first instruction
        # is MRS. Only actual identity-body system reads are modeled here.
        word=int.from_bytes(cpu.mem_read(address,4),'little')
        if word&0xffffffe0 in (0xd5384240,0xd53e1000):
            cpu.reg_write(UC_ARM64_REG_X0+(word&31),12 if word&0xffffffe0==0xd5384240 else 0);cpu.reg_write(UC_ARM64_REG_PC,address+4)
    def read(self,cpu,access,address,size,value,unused):
        if 0x03006200<=address<0x03006210:
            assert self.publications,'SID reached before session publication'
            self.sid.append(address)
    def call(self,name,*arguments):
        for index,value in enumerate(arguments):self.cpu.reg_write(UC_ARM64_REG_X0+index,value)
        self.cpu.reg_write(UC_ARM64_REG_SP,0x50ffeff0);self.cpu.reg_write(UC_ARM64_REG_LR,0x50fff000);self.finished=False
        self.cpu.emu_start(self.symbols[name],0,count=2000000)
        assert self.finished,(name,'instruction allowance')
        return self.cpu.reg_read(UC_ARM64_REG_X0)

def run(out,source,step):
    board=out/'build/sunstone-a733-board.elf';bluetooth=out/'build/sunstone-a733-bluetooth.elf'
    session=(out/'bt-session.bin').read_bytes();profile=(out/'bt-profile.bin').read_bytes()
    for kind,image in [('board',board),('bluetooth',bluetooth)]:
        m=Machine(image,kind,out,source,session,profile)
        (out/(kind+'-validator-preflight.json')).write_text(json.dumps({'passed':True,'native_session_validator':True,'native_contract_validator':True,
            'native_bluetooth_boot_validator':kind=='bluetooth','linked_build_id':m.identity.hex(),'bound_session_sha256':hashlib.sha256(m.raw).hexdigest(),
            'profile_sha256':hashlib.sha256(profile).hexdigest(),'physical_authority':'Modeled; nondeployable.'},indent=2)+'\n')
    for size in (592,824,856,591,4097):
        label='seal-span-'+str(size)
        if step(label):continue
        m=Machine(board,'board',out,source,session,profile)
        # Opaque suffix bytes come from the retained real profile. These cases
        # test prefix sealing after caller admission, not subclass policy.
        full=bytes(m.raw)+profile[:max(0,min(size,4096)-592)]
        struct.pack_into('<Q',m.contract,184,size);m.contract[192:224]=hashlib.sha256(full[:size]).digest()
        m.cpu.mem_write(0x43200000,bytes(m.contract));m.cpu.mem_write(0x41f00000,full)
        result=m.call('board_session_prepare',0x43200000)
        (out/(label+'-observation.json')).write_text(json.dumps({'result':result,'publications':m.publications,'hashes':m.hashes,
            'source_sha256':hashlib.sha256(m.cpu.mem_read(0x41f00000,min(size,4096))).hexdigest(),
            'contract_digest':bytes(m.cpu.mem_read(0x43200000+192,32)).hex(),'trace':list(m.trace)},indent=2)+'\n')
        assert result==int(size in (592,824,856)),(label,result,m.publications,m.hashes)
        assert bool(m.call('board_session_snapshot'))==(size in (592,824,856))
        step(label,{'publications':m.publications,'opaque_authenticated_span':size})
    for variant in ('prefix-tamper','suffix-tamper','publication-failure','retained-prefix'):
        label='seal-'+variant
        if step(label):continue
        m=Machine(board,'board',out,source,session,profile,1 if variant=='publication-failure' else 0)
        if variant=='prefix-tamper':m.cpu.mem_write(0x41f00000+456,struct.pack('<Q',999))
        if variant=='suffix-tamper':
            full=bytes(m.raw)+profile[:232];struct.pack_into('<Q',m.contract,184,len(full));m.contract[192:224]=hashlib.sha256(full).digest();m.cpu.mem_write(0x43200000,bytes(m.contract));m.cpu.mem_write(0x41f00000,full);m.cpu.mem_write(0x41f00000+600,b'\xff')
        assert m.call('board_session_prepare',0x43200000)==int(variant=='retained-prefix')
        pointer=m.call('board_session_snapshot')
        if variant=='retained-prefix':
            m.cpu.mem_write(0x41f00000+400,struct.pack('<I',3));m.cpu.mem_write(0x41f00000+456,struct.pack('<Q',999))
            assert bytes(m.cpu.mem_read(pointer,592))==m.raw
            m.cpu.mem_write(m.symbols['a733_cached_runtime'],struct.pack('<3Q',struct.unpack_from('<Q',m.raw,24)[0],24000024,1440000024))
            reached=[]
            def provider(address):
                if address==m.symbols['a733_smp_device_preparation_admitted']:m.returned(1);return True
                if address==m.symbols['a733_native_usb_init']:
                    fields=m.layouts['board_application'];assert bytes(m.cpu.mem_read(m.symbols['board']+fields['admitted_session'],592))==m.raw
                    reached.append(True);m.finished=True;m.cpu.emu_stop();return True
                if address==m.symbols['platform_panic']:raise AssertionError('Unexpected board startup refusal')
                return False
            m.extra=provider;m.call('a733_smp_diagnostic_start');assert reached
        else:assert not pointer and not m.sid
        step(label,{'publications':m.publications,'retained_prefix':bool(pointer)})
    for variant in ('nominal-retained','profile-tamper','header-oversize','profile-publication-failure'):
        label='bluetooth-'+variant
        if step(label):continue
        m=Machine(bluetooth,'bluetooth',out,source,session,profile,2 if variant=='profile-publication-failure' else 0)
        assert m.call('board_session_prepare',0x43200000)==1
        if variant=='profile-tamper':m.cpu.mem_write(0x43000000+24,struct.pack('<Q',999))
        if variant=='header-oversize':m.cpu.mem_write(0x43000000+12,struct.pack('<I',4097))
        # Sealed authority, rather than changed live workload/SID, admits reads.
        m.cpu.mem_write(0x41f00000+400,struct.pack('<I',3));m.cpu.mem_write(0x41f00000+312,struct.pack('<Q',999))
        assert m.call('board_identity_prepare',24000024)==int(variant=='nominal-retained')
        pointer=m.call('bluetooth_admitted_profile')
        if variant=='nominal-retained':
            assert len(m.sid)==4 and bytes(m.cpu.mem_read(pointer,len(profile)))==profile
            assert m.publications[1][0]==pointer,'Different profile published before SID'
            m.cpu.mem_write(0x43000000,bytes(len(profile)))
            assert m.call('bluetooth_boot_profile',m.call('board_session_snapshot'),pointer)==1
            m.cpu.mem_write(m.symbols['board'],bytes(m.raw))
            assert m.call('board_application_pins',0x43300000)==1
        else:assert not pointer and not m.sid
        step(label,{'publications':m.publications,'sid_reads':m.sid,'retained_profile':bool(pointer)})
    spec=importlib.util.spec_from_file_location('network_profile_producer',source/'checks/board-supervised-host.py');producer=importlib.util.module_from_spec(spec);spec.loader.exec_module(producer)
    _,network=producer.session(3)
    for variant in ('live-change-after-copy','tamper-before-copy'):
        label='network-'+variant
        if step(label):continue
        m=Machine(board,'board',out,source,session,profile)
        raw=bytearray(m.raw);struct.pack_into('<2Q',raw,352,0x43000000,len(network));raw[368:400]=hashlib.sha256(network).digest()
        m.cpu.mem_write(m.symbols['board'],bytes(raw));m.cpu.mem_write(0x43000000,network)
        reached=[];hashes=[]
        def boundary(address):
            if address==m.symbols['storage_digest_bytes']:
                pointer=m.cpu.reg_read(UC_ARM64_REG_X0)
                assert pointer!=0x43000000 and bytes(m.cpu.mem_read(pointer,len(network)))==(network if variant=='live-change-after-copy' else bytes([network[0]^1])+network[1:])
                hashes.append(pointer)
                if variant=='live-change-after-copy':m.cpu.mem_write(0x43000000,bytes(len(network)))
            if address==m.symbols['board_radio_profile_runtime']:
                pointer=m.cpu.reg_read(UC_ARM64_REG_X1)
                assert hashes==[pointer] and bytes(m.cpu.mem_read(pointer,len(network)))==network
                reached.append(pointer);m.returned(0);return True
            return False
        m.extra=boundary
        if variant=='tamper-before-copy':m.cpu.mem_write(0x43000000,bytes([network[0]^1]))
        assert m.call('radio_profile')==0
        assert bool(reached)==(variant=='live-change-after-copy')
        step(label,{'private_hash_inputs':hashes,'runtime_source':reached,'scope':'Actual copy/hash binding; stop at existing runtime derivation boundary, not network startup.'})
