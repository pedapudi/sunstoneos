#!/usr/bin/env python3
"""Validate copied GPU results only after the host observes a new ROM instance."""
import struct,re
PACKET_MAGIC=0x3154455255504741
RECEIPT_MAGIC=0x4750555245433031
def require(condition,message):
 if not condition:raise ValueError(message)
def validate_reports(monitor,platform,receipt,packet,p):
 require((len(monitor),len(platform),len(receipt),len(packet))==(128,16384,104,320),'Incomplete GPU returned reports.')
 m=struct.unpack('<16Q',monitor);h=struct.unpack_from('<16Q',platform);r=struct.unpack('<13Q',receipt)
 magic,version,size,epoch,generation,first,second,count,lines=struct.unpack_from('<QIIQQ4I',packet)
 words=struct.unpack_from('<64I',packet,48);start,end=struct.unpack_from('<2Q',packet,304);g=p['geometry']
 expected=tuple(((17+(i+1)*3)^(0x9e3779b9+(i<<16)))&0xffffffff for i in range(32))*2
 require(m[:3]==(0x4153373347535232,p['epoch'],p['generation']) and m[3:6]==(5,0,0) and m[7]==1,'No matched healthy GPU monitor completion.')
 require((magic,version,size,epoch,generation,first,second,count)==(PACKET_MAGIC,1,320,p['epoch'],p['generation'],1,2,64) and words==expected,'GPU packet identity/sequence/arithmetic differs.')
 clean_lines=(g['clean_end']-g['clean_start']+g['wb_end']-g['wb_start'])//64
 require(r[:6]==(RECEIPT_MAGIC,p['epoch'],p['generation'],p['lease'],0,1) and r[6:11]==(g['clean_start'],g['clean_end'],g['wb_start'],g['wb_end'],clean_lines) and lines==clean_lines,'GPU receipt ownership/publication extent differs.')
 deadline=r[11];require(m[6]<=start<=end==m[8]<deadline<=m[6]+60*24000000,'GPU cleanup counter/deadline differs.')
 require(h[:3]==(0x534b4552,5,0) and h[3]==r[12] and h[3]<=16127 and h[4:8]==(0,0,0,0) and m[6]<=h[8]<=h[9]==m[8],'GPU platform completion/exception/log differs.')
 require(platform[256+h[3]]==0,'Unterminated GPU log.');text=platform[256:256+h[3]].decode('ascii')
 pattern=r'GPU_STAGE=STOPPED\nGPU_COMPUTE_WORDS=\nGPU_SEQUENCE=0x0000000000000001\n((?:0x[0-9a-f]{16}\n){32})GPU_SEQUENCE=0x0000000000000002\n((?:0x[0-9a-f]{16}\n){32})PASS GPU two copied results, CLOSE and parked providers\n'
 matches=list(re.finditer(pattern,text));require(len(matches)==1 and matches[0].end()==len(text),'Missing or ambiguous GPU stopped/copied-output terminal log.')
 copied=tuple(int(x,16) for section in (matches[0][1],matches[0][2]) for x in section.splitlines());require(copied==words,'GPU log and protected packet disagree.')
 return {'normal_return':True,'functional_success':True,'status':0,'sequence':[first,second],'words':list(words),'clean_lines':lines,'cleanup_start':start,'cleanup_end':end,'diagnostic_deadline':deadline,'log':text}
