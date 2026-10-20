"""Fixed full-HD boot and image geometry, using the existing extended-memory ABI."""
import hashlib
from pathlib import Path
import struct
from elftools.elf.elffile import ELFFile

def require(ok,message):
    if not ok:raise ValueError(message)

def request_bytes(address,identity,raw,epoch,generation,lease):
    reservation=struct.unpack_from('<Q',raw,200)[0] # Existing board memory authority.
    require(len(raw)==856 and struct.unpack_from('<Q',raw,600)[0] in (3,4) and len(identity)==32 and
        0x41000000<=address<=0x411fffe0 and all(type(x) is int and 0<x<2**64 for x in (epoch,generation,lease,reservation)),
        'Invalid full-HD launch identity.')
    session_digest=hashlib.sha256(raw).digest()
    request=struct.pack('<8Q',0x4153373348324c31,2,128,epoch,generation,generation^((1<<64)-1),lease,address)+identity+session_digest
    contract=struct.pack('<6Q',0,2,288,epoch,generation,generation^((1<<64)-1))+identity
    contract+=struct.pack('<18Q',0,0,0,0,lease,0,0,0,0,0,0,0,1,reservation,0x48000000,0x78000000,0x78000000,0x79000000)
    contract+=session_digest+raw[80:112]
    require(len(contract)==288,'Full-HD contract ABI differs.')
    return request,contract+bytes(4096-len(contract))

def image_parts(path,high=False):
    # Same fixed large-profile ELF geometry as apps/gpu-render/prepare.py.
    with Path(path).open('rb') as f:
        elf=ELFFile(f);require(elf['e_machine']=='EM_AARCH64' and elf['e_entry']==0x41000000,'Native full-HD ELF required.')
        symbols={s.name:(s['st_value'],s['st_size']) for s in elf.get_section_by_name('.symtab').iter_symbols()}
        parts=[]
        for s in elf.iter_segments():
            if s['p_type']!='PT_LOAD':continue
            a,n=s['p_paddr'],s['p_memsz'];data=s.data()
            require(a==s['p_vaddr'] and n>0 and len(data)<=n and not (s['p_flags']&3==3) and
                all(a>=p['address']+p['memory_bytes'] or p['address']>=a+n for p in parts),'Invalid full-HD ELF allocations.')
            require((0x41000000<=a<a+n<=0x42200000 and (not data or a+len(data)<=0x42000000)) or
                (not data and (a,a+n) in [(0x48000000,0x78000000),(0x78000000,0x79000000)]),'Full-HD ELF span outside reservations.')
            require(not s['p_flags']&1 or a+n<=0x41200000,'Full-HD code outside image prefix.')
            parts.append({'address':a,'memory_bytes':n,'data':data,'flags':int(s['p_flags'])})
        for name,value in {'__device_pool_start':0x48000000,'__device_pool_end':0x78000000,'__uncached_pool_start':0x78000000,'__uncached_pool_end':0x79000000,'__a733_smp_contract':0x42004000,'__a733_smp_contract_end':0x42005000}.items():
            require(symbols.get(name,(0,))[0]==value,'Full-HD linker ownership differs.')
        require('__bss_end' in symbols and symbols['__bss_end'][0]<=0x41f00000,'Full-HD session overlaps BSS.')
        def immutable(name,size):
            address,n=symbols.get(name,(0,0));require(n==size,'Full-HD immutable marker absent: '+name)
            match=[p for p in parts if not p['flags']&2 and p['address']<=address and address+size<=p['address']+len(p['data'])]
            require(len(match)==1,'Full-HD marker is not immutable/file-backed.')
            p=match[0];return address,p['data'][address-p['address']:address-p['address']+size]
        _,geometry=immutable('h264_fullhd_geometry',64)
        require(struct.unpack('<8Q',geometry)==(0x4844464844323634,1,1920,1088,1920,1080,4194304,3110400),'Full-HD compiled geometry differs.')
        require(('h264_high_geometry' in symbols)==high,'Extended image/profile selection differs.')
        if high:
            _,marker=immutable('h264_high_geometry',64)
            require(struct.unpack('<8Q',marker)==(0x4847494844323634,1,1920,1088,5,5,14749696,1048576),'High compiled capacity marker differs.')
        address,identity=immutable('a733_smp_build_id',32)
        for p in parts:p.pop('flags')
        return parts,address,identity
