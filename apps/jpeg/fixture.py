"""Derive an exact zero-coefficient diagnostic from the preserved JPEG metadata."""
import hashlib
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
