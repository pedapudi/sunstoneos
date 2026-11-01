"""Host admission and records for the native probe ABI; no device access."""
import hashlib
import io
from pathlib import Path
import secrets
import struct
from elftools.elf.elffile import ELFFile

MAGIC=0x3145424f52505353
SESSION_MAGIC=0x3153455342505353
RESULT_MAGIC=0x3153455242505353
COMPLETE=0x454e4f4442505353
SESSION=0x42004000
RECEIPT=0x42000080
FIELDS=('magic','version','bytes','watchdog_mode','seconds','session_address','session_bytes',
        'result_address','result_bytes','payload_address','payload_bytes','input_address','input_bytes','flags')
RESULT_FIELDS=('magic','version','bytes','epoch','generation','lease','status','payload_bytes','payload_hash',
               'cleanup','retired','start_ticks','end_ticks','failure_site','watchdog_mode','complete')


def require(ok,text):
    if not ok:raise ValueError(text)


def fnv(data):
    value=0xcbf29ce484222325
    for byte in data:value=((value^byte)*0x100000001b3)&((1<<64)-1)
    return value


def inspect(image):
    data=Path(image).read_bytes();elf=ELFFile(io.BytesIO(data))
    require(elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,'Probe entry must be AArch64 0x41000000.')
    table=elf.get_section_by_name('.symtab');symbols={}
    for symbol in table.iter_symbols() if table else ():
        if symbol.name.startswith('sunstone_probe_') and symbol['st_shndx']!='SHN_UNDEF':
            require(symbol.name not in symbols,'Ambiguous probe symbol.');symbols[symbol.name]=symbol
    marker=symbols.get('sunstone_probe_marker')
    require(marker is not None and marker['st_size']==112 and type(marker['st_shndx']) is int,'Missing immutable native probe marker.')
    section=elf.get_section(marker['st_shndx']);offset=marker['st_value']-section['sh_addr']
    require(section['sh_type']=='SHT_PROGBITS' and section['sh_flags']&2 and not section['sh_flags']&1 and
            0<=offset<=section['sh_size']-112,'Probe marker must be immutable allocated data.')
    require(any(p['p_type']=='PT_LOAD' and p['p_vaddr']<=marker['st_value'] and marker['st_value']+112<=p['p_vaddr']+p['p_filesz'] for p in elf.iter_segments()),'Probe marker has no file-backed load coverage.')
    m=dict(zip(FIELDS,struct.unpack('<14Q',section.data()[offset:offset+112])))
    require((m['magic'],m['version'],m['bytes'])==(MAGIC,1,112),'Unsupported native probe ABI.')
    require(m['flags']==1,'Probe requires power removal or lacks normal-return capability.')
    require(m['watchdog_mode']==0xb1 and 1<=m['seconds']<=600 and
            (m['session_address'],m['session_bytes'],m['result_address'],m['result_bytes'])==
            (SESSION,128,RECEIPT,128),'Probe watchdog or session/receipt geometry differs.')
    uploads=[]
    for p in elf.iter_segments():
        if p['p_type']!='PT_LOAD' or not p['p_memsz']:continue
        low,high=p['p_paddr'],p['p_paddr']+p['p_memsz']
        require(low==p['p_vaddr'] and 0x41000000<=low<high<=0x41f00000 and
                p['p_filesz']<=p['p_memsz'] and p['p_flags']&3!=3,'Probe load segment exceeds image RAM.')
        require(not any(low<b and a<high for a,b in [(u['address'],u['end']) for u in uploads]),'Probe segments overlap.')
        uploads.append({'address':low,'end':high,'data':p.data()})
    require(uploads and any(p['address']<=0x41000000< p['address']+len(p['data']) for p in uploads),'Probe entry has no image bytes.')
    for name in ('begin','complete','return','fault','quiesce','restore_interrupts','rom'):
        s=symbols.get('sunstone_probe_'+name)
        require(s is not None and s['st_info']['bind']=='STB_GLOBAL' and s['st_info']['type']=='STT_FUNC' and
                s['st_size']>0 and type(s['st_shndx']) is int,'Probe requires a strong implementation of '+name+'.')
        section=elf.get_section(s['st_shndx'])
        require(section['sh_type']=='SHT_PROGBITS' and section['sh_flags']&6==6 and not section['sh_flags']&1 and section['sh_addr']<=s['st_value'] and s['st_value']+s['st_size']<=section['sh_addr']+section['sh_size'] and any(p['p_type']=='PT_LOAD' and p['p_flags']&7==5 and p['p_vaddr']<=s['st_value'] and s['st_value']+s['st_size']<=p['p_vaddr']+p['p_filesz'] for p in elf.iter_segments()),
                'Probe helper must reside in allocated executable code.')
    def object_at(name,address,size):
        s=symbols.get(name)
        require(s is not None and s['st_info']['bind']=='STB_GLOBAL' and s['st_info']['type']=='STT_OBJECT' and
                (s['st_value'],s['st_size'])==(address,size) and type(s['st_shndx']) is int,'Probe object differs: '+name)
        require(0x41000000<=address<address+size<=0x41f00000 and any(p['p_type']=='PT_LOAD' and p['p_flags']&7==6 and p['p_vaddr']<=address and address+size<=p['p_vaddr']+p['p_memsz'] for p in elf.iter_segments()),'Probe object lacks writable image-RAM load coverage.')
        section=elf.get_section(s['st_shndx'])
        require(section['sh_flags']&3==3 and section['sh_addr']<=address and address+size<=section['sh_addr']+section['sh_size'],
                'Probe object is outside allocated writable storage.')
    p,n=m['payload_address'],m['payload_bytes']
    require(0<n<=16*1024*1024,'Invalid probe payload capacity.')
    if not (0x42000100<=p<p+n<=0x42004000):
        require(0x41000000<=p<p+n<=0x41f00000,'Probe payload is outside admitted RAM.')
        object_at('sunstone_probe_payload',p,n)
    if m['input_bytes']:
        require(m['input_bytes']<=1024*1024,'Probe input too large.')
        object_at('sunstone_probe_input',m['input_address'],m['input_bytes'])
        require(not (p<m['input_address']+m['input_bytes'] and m['input_address']<p+n),'Probe input overlaps output.')
    else:require(m['input_address']==0,'Empty probe input must have address zero.')
    return {'marker':m,'uploads':uploads,'watchdog':{'mode':m['watchdog_mode'],'session_seconds':m['seconds']}}


def prepare(image,info,inputs,out):
    m=info['marker'];require(len(inputs)==(1 if m['input_bytes'] else 0),'Probe input count differs from its marker.')
    value=Path(inputs[0]).read_bytes() if inputs else b''
    require(len(value)==m['input_bytes'],'Probe input length differs from its marker.')
    out.mkdir(parents=True,exist_ok=False)
    epoch,generation,lease=[secrets.randbits(62)|1 for _ in range(3)]
    digest=hashlib.sha256(Path(image).read_bytes()).digest()
    raw=struct.pack('<9Q',SESSION_MAGIC,1,128,epoch,generation,lease,m['seconds'],len(value),fnv(value))+digest+bytes(24)
    (out/'session.bin').write_bytes(raw)
    uploads=[]
    for i,p in enumerate(info['uploads']):
        if p['data']:
            path=out/f'segment-{i}.bin';path.write_bytes(p['data']);uploads.append((p['address'],path))
    if value:
        path=out/'input.bin';path.write_bytes(value);uploads.append((m['input_address'],path))
    for name,address,data in [('payload-zero',m['payload_address'],bytes(m['payload_bytes'])),
                              ('receipt-zero',RECEIPT,bytes(128)),('mailbox-zero',SESSION+128,bytes(24)),
                              ('session',SESSION,raw)]:
        path=out/(name+'.bin');path.write_bytes(data);uploads.append((address,path))
    return {'epoch':epoch,'generation':generation,'lease':lease,'format':'native-probe','marker':m,
            'watchdog':info['watchdog'],'launch':['reset64','0x41000000'],'uploads':uploads}


def receipt(raw,plan):
    require(len(raw)==128,'Truncated probe receipt.')
    r=dict(zip(RESULT_FIELDS,struct.unpack('<16Q',raw)))
    require((r['magic'],r['version'],r['bytes'],r['epoch'],r['generation'],r['lease'],r['complete'])==
            (RESULT_MAGIC,1,128,plan['epoch'],plan['generation'],plan['lease'],COMPLETE),'Probe receipt is incomplete or belongs to another session.')
    require(r['cleanup']==7 and r['retired']==0xfe and not r['failure_site'] and r['watchdog_mode']==0,
            'Probe receipt lacks complete cleanup/retirement.')
    require(r['payload_bytes']<=plan['marker']['payload_bytes'] and r['start_ticks']<=r['end_ticks'] and
            r['end_ticks']-r['start_ticks']<=plan['watchdog']['session_seconds']*24000000,'Probe receipt exceeds payload/time bounds.')
    return r
