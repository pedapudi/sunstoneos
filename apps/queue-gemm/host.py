#!/usr/bin/env python3
"""Validate the complete bounded local-memory GEMM result."""
import struct

def expected_checksum(sequence=4):
    return sum(sum(((row*3+k+sequence)%7-3)*((k+col*5+sequence*2)%9-4)
                   for k in range(19)) for row in range(13) for col in range(11)) & 0xffffffff

def decode(payload):
    if len(payload)!=48:raise ValueError('queue-gemm result must contain 48 bytes')
    jobs,rows,cols,depth,checksum,sequence,local_bytes,guards,*times=struct.unpack('<12I',payload)
    if (jobs,rows,cols,depth,checksum,sequence,guards)!=(4,13,11,19,expected_checksum(),4,32) or local_bytes not in (0,384):
        raise ValueError('queue-gemm completion, geometry, checksum or local allocation differs')
    if any(t>=200000 for t in times):raise ValueError('queue-gemm timing exceeds job budget')
    return {'jobs':jobs,'rows':rows,'columns':cols,'depth':depth,'checksum':checksum,
            'sequence':sequence,'local_bytes':local_bytes,'guards':guards}
