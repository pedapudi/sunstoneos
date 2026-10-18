"""Derive an exact zero-coefficient diagnostic from the preserved JPEG metadata."""
import hashlib
import json
from pathlib import Path
SOURCE_SHA256='6522f2719f1f05e23a95aa71a18d52f2e91b19b4b6c94381b4100de4c79fd573'
def build(source):
    if hashlib.sha256(source).hexdigest()!=SOURCE_SHA256:raise ValueError('Wrong preserved JPEG source')
    offset=2;parts=[];frame=None
    while offset<len(source):
        if source[offset]!=255:raise ValueError('Malformed marker source')
        marker=source[offset+1];length=int.from_bytes(source[offset+2:offset+4],'big')
        segment=source[offset:offset+2+length];offset+=2+length
        if marker==0xda:break
        if marker==0xdb:parts.append(segment)
        if marker==0xc0:frame=segment
    if frame is None or frame[4:10]!=bytes([8,0,32,0,48,3]):raise ValueError('Wrong fixed baseline frame')
    # One canonical code0 of length1: DC category0 and AC EOB. Every coefficient
    # is exactlyzero in all36blocks; inverse transform is zero independently of
    # quantization and IDCT rounding. JPEG's unsigned level shift yields128.
    huffman=bytes([0,1])+bytes(15)+b'\x00'+bytes([16,1])+bytes(15)+b'\x00'
    dht=b'\xff\xc4'+(len(huffman)+2).to_bytes(2,'big')+huffman
    sos=b'\xff\xda\x00\x0c\x03\x01\x00\x02\x00\x03\x00\x00\x3f\x00'
    return b'\xff\xd8'+b''.join(parts)+frame+dht+sos+bytes(9)+b'\xff\xd9'

# The analytic oracle indexes plane pixels directly. It does not traverse JPEG
# scan order, zigzag positions, DC predictors or Huffman/quantization tables.
def block_values(plane,x,y):
    if plane==0:return -44+12*x+16*y,2+2*(x%3),3+2*(y%2),2 if (x+y)%2==0 else -2
    if plane==1:return -48+17*x+11*y,5+x,2+2*y,-3
    return 31-13*x+7*y,-4-x,5-y,3

def nonconstant_reference():
    signs=(1,-1,-1,1,1,-1,-1,1)
    out=bytearray()
    for plane,(width,height) in enumerate(((48,32),(24,16),(24,16))):
        for y in range(height):
            for x in range(width):
                dc,horizontal,vertical,product=block_values(plane,x//8,y//8)
                value=128+dc+horizontal*signs[x%8]+vertical*signs[y%8]+product*signs[x%8]*signs[y%8]
                if not 0<value<255:raise ValueError('Fixture must avoid clipping')
                out.append(value)
    return bytes(out)

def nonconstant():
    """Self-generated baseline420 with exactly representable inverse samples."""
    def segment(marker,payload):return bytes((255,marker))+(len(payload)+2).to_bytes(2,'big')+payload
    zigzag=[]
    for diagonal in range(15):
        coords=[(x,diagonal-x) for x in range(8) if 0<=diagonal-x<8]
        if diagonal%2==1:coords.reverse()
        zigzag.extend(y*8+x for x,y in coords)
    if len(set(zigzag))!=64:raise ValueError('Invalid zigzag')
    quant=[[1+(i*3+5)%15 for i in range(64)],[1+(i*7+2)%13 for i in range(64)]]
    for q,values in zip(quant,((8,4,8,2),(4,8,2,4))):
        for index,value in zip((0,4,32,36),values):q[index]=value
    blocks=[]
    for my in range(2):
        for mx in range(3):
            for plane,x,y in ((0,2*mx,2*my),(0,2*mx+1,2*my),(0,2*mx,2*my+1),(0,2*mx+1,2*my+1),(1,mx,my),(2,mx,my)):
                coefficients=[0]*64
                for index,value in zip((0,4,32,36),block_values(plane,x,y)):
                    numerator=8*value;q=quant[0 if plane==0 else 1][index]
                    if numerator%q:raise ValueError('Coefficient is not exactly quantizable')
                    coefficients[index]=numerator//q
                blocks.append((plane,[coefficients[index] for index in zigzag]))
    ac_symbols={0,0xf0}
    for unused,coefficients in blocks:
        run=0
        for value in coefficients[1:]:
            if not value:run+=1;continue
            while run>=16:run-=16
            ac_symbols.add(run*16+abs(value).bit_length());run=0
    def codes(symbols,length):
        if len(symbols)>=(1<<length):raise ValueError('Huffman all-ones code is forbidden')
        counts=bytearray(16);counts[length-1]=len(symbols)
        return bytes(counts)+bytes(symbols),{symbol:(index,length) for index,symbol in enumerate(symbols)}
    tables={};dht=bytearray()
    for kind in (0,1):
        for table in (0,1):
            symbols=list(range(12)) if kind==0 else sorted(ac_symbols)
            if table:symbols.reverse()
            payload,mapping=codes(symbols,(4 if kind==0 else 6)+table)
            dht.extend(bytes((kind*16+table,))+payload);tables[kind,table]=mapping
    bits=[]
    def put(value,count):bits.extend((value>>shift)&1 for shift in range(count-1,-1,-1))
    def symbol(kind,table,value):
        code,length=tables[kind,table][value];put(code,length)
    def amplitude(value):
        count=abs(value).bit_length()
        put(value if value>=0 else value+(1<<count)-1,count)
        return count
    predictors=[0,0,0]
    for plane,coefficients in blocks:
        table=0 if plane==0 else 1;difference=coefficients[0]-predictors[plane];predictors[plane]=coefficients[0]
        symbol(0,table,abs(difference).bit_length());amplitude(difference)
        run=0
        for value in coefficients[1:]:
            if not value:run+=1;continue
            while run>=16:symbol(1,table,0xf0);run-=16
            symbol(1,table,run*16+abs(value).bit_length());amplitude(value);run=0
        if run:symbol(1,table,0)
    bits.extend([1]*((-len(bits))%8));entropy=bytearray()
    for offset in range(0,len(bits),8):
        value=sum(bit<<(7-i) for i,bit in enumerate(bits[offset:offset+8]));entropy.append(value)
        if value==255:entropy.append(0)
    dqt=b''.join(segment(0xdb,bytes((i,))+bytes(q[index] for index in zigzag)) for i,q in enumerate(quant))
    frame=segment(0xc0,bytes((8,0,32,0,48,3,1,0x22,0,2,0x11,1,3,0x11,1)))
    scan=segment(0xda,bytes((3,1,0,2,0x11,3,0x11,0,63,0)))
    return b'\xff\xd8'+dqt+frame+segment(0xc4,dht)+scan+entropy+b'\xff\xd9'

_PROFILES=tuple(json.loads(Path(__file__).with_name('fixtures.json').read_text()))

def profiles():
    return [dict(profile) for profile in _PROFILES]

def profile_for(fixture_hash,reference_hash):
    matches=[p for p in profiles() if (p['fixture_sha256'],p['reference_sha256'])==(fixture_hash,reference_hash)]
    if len(matches)!=1:raise ValueError('Unsupported or crossed JPEG fixture/reference pair')
    return matches[0]
