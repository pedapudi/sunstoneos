#!/usr/bin/env python3
"""CPU reference for the single-inference test network, and its comparison.

The test network is the catalogue entry conv12 (tools/npu-models.json): one
3x3 convolution, stride 1, no padding, from a 1x3x12x12 UINT8 input to a
1x4x10x10 UINT8 output, both NCHW. It was compiled from the integer formulas
of tools/npu-conv-fixture.py (--width 12 --channels 4); the quantization below
is the compiler's recorded conv3x3_uint8.quantize (SHA-256 5304af56...).

`reference` regenerates the input and computes the output in integer
arithmetic on the CPU, then checks both against the bytes linked into the
image: the input the NPU reads and the SDK simulator output the image compares
against. `compare` checks a returned device output against the CPU reference.
Neither operation touches a board. Pure Python; no NumPy.
"""
import argparse,hashlib,json,math,re,sys
from pathlib import Path
from elftools.elf.elffile import ELFFile

MODEL_SHA256='d8de2e7a886741959d7806783e0a4bf96addc726f6c18804a420bbf4a250d3d9'
WIDTH,CHANNELS=12,4
OUT=WIDTH-2
# (scale, zero point), UINT8 asymmetric affine, round half to even.
INPUT_Q=(0.01568627543747425,127)
WEIGHT_Q=(0.0019607844296842813,127)
BIAS_SCALE=3.075740460189990e-05      # INT32, zero point 0
OUTPUT_Q=(0.01930147036910057,122)

def rtne(x):
    f=math.floor(x);d=x-f
    return f+1 if d>0.5 or (d==0.5 and f%2) else f

def quantize(value,q):return max(0,min(255,rtne(value/q[0])+q[1]))

def network():
    """The float network of tools/npu-conv-fixture.py, as dyadic fractions."""
    x=[(((i*7)%33)-16)/8 for i in range(3*WIDTH*WIDTH)]
    w=[((i%5)-2)/8 for i in range(CHANNELS*27)]
    b=[v/64 for v in ([16,-32]*CHANNELS)[:CHANNELS]]
    return x,w,b

def convolve(x,w,b,zero=(0,0)):
    out=[]
    for ch in range(CHANNELS):
        for y in range(OUT):
            for col in range(OUT):
                total=b[ch]
                for z in range(3):
                    for ky in range(3):
                        for kx in range(3):
                            total+=(x[z*WIDTH*WIDTH+(y+ky)*WIDTH+col+kx]-zero[0])*(w[ch*27+z*9+ky*3+kx]-zero[1])
                out.append(total)
    return out

def reference():
    """Quantized input bytes, quantized output bytes and the float output."""
    x,w,b=network()
    qx=[quantize(v,INPUT_Q) for v in x];qw=[quantize(v,WEIGHT_Q) for v in w]
    qb=[rtne(v/BIAS_SCALE) for v in b]
    accumulator=convolve(qx,qw,qb,(INPUT_Q[1],WEIGHT_Q[1]))
    qy=[max(0,min(255,rtne(a*BIAS_SCALE/OUTPUT_Q[0])+OUTPUT_Q[1])) for a in accumulator]
    return bytes(qx),bytes(qy),convolve(x,w,b)

def linked(image):
    elf=ELFFile(open(image,'rb'));symbols={s.name:s for s in elf.get_section_by_name('.symtab').iter_symbols()}
    def read(name):
        s=symbols[name];at,n=int(s['st_value']),int(s['st_size'])
        for seg in elf.iter_segments():
            if seg['p_type']=='PT_LOAD' and seg['p_vaddr']<=at and at+n<=seg['p_vaddr']+seg['p_filesz']:
                return seg.data()[at-seg['p_vaddr']:at-seg['p_vaddr']+n]
        raise ValueError('Unbacked symbol '+name)
    return {n:read('a733_npu_application_'+n) for n in ('model','input','reference')}

def compare(output,expected,real):
    """Byte agreement with the CPU reference, plus the dequantized error
    against the unquantized float network for context."""
    if len(output)!=len(expected):raise ValueError('Output has %d bytes, reference %d'%(len(output),len(expected)))
    diff=[abs(a-b) for a,b in zip(output,expected)]
    error=[abs((v-OUTPUT_Q[1])*OUTPUT_Q[0]-r) for v,r in zip(output,real)]
    first=next((i for i,d in enumerate(diff) if d),None)
    return {'match':not any(diff),'bytes':len(output),'mismatched_bytes':sum(1 for d in diff if d),
            'max_code_difference':max(diff),'first_mismatch':None if first is None else
            {'index':first,'channel':first//(OUT*OUT),'y':first%(OUT*OUT)//OUT,'x':first%OUT,
             'device':output[first],'reference':expected[first]},
            'max_float_error':max(error),'output_sha256':hashlib.sha256(output).hexdigest(),
            'reference_sha256':hashlib.sha256(expected).hexdigest()}

def device_output(path):
    """The copied output from a host result: a session record holding
    npu_result, the npu_result object itself, or the returned log text."""
    text=Path(path).read_text()
    try:value=json.loads(text)
    except ValueError:value=None
    if isinstance(value,dict):
        value=value.get('npu_result',value)
        if 'output' in value:return bytes.fromhex(value['output'])
        text=value.get('log','')
    found=re.findall(r'NPU_OUTPUT=\n((?:0x[0-9a-f]{16}\n)+)',text)
    if len(found)!=1:raise ValueError('No single NPU_OUTPUT block in '+str(path))
    data=[int(v,16) for v in found[0].split()]
    if any(v>255 for v in data):raise ValueError('Invalid output byte')
    return bytes(data)

def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    sub=p.add_subparsers(dest='operation',required=True)
    r=sub.add_parser('reference',help='check the CPU reference against the image-linked input and SDK output')
    r.add_argument('--image',type=Path,required=True);r.add_argument('--output',type=Path)
    c=sub.add_parser('compare',help='compare a returned device output with the CPU reference')
    c.add_argument('--image',type=Path,required=True);c.add_argument('--result',type=Path,required=True)
    a=p.parse_args(argv)
    qx,qy,real=reference();blobs=linked(a.image)
    if hashlib.sha256(blobs['model']).hexdigest()!=MODEL_SHA256:
        raise SystemExit('The image links another network; this reference covers conv12 only.')
    if blobs['input']!=qx:raise SystemExit('Image input differs from the regenerated quantized input.')
    sdk=compare(blobs['reference'],qy,real)
    if a.operation=='reference':
        report={'network':'conv12','input_sha256':hashlib.sha256(qx).hexdigest(),'cpu_reference':qy.hex(),
                'sdk_reference_agreement':sdk}
        if a.output:a.output.write_text(json.dumps(report,indent=2)+'\n')
        print(json.dumps({k:v for k,v in report.items() if k!='cpu_reference'},indent=2))
        return 0 if sdk['match'] else 1
    result=compare(device_output(a.result),qy,real);result['sdk_reference_agreement']=sdk['match']
    print(json.dumps(result,indent=2))
    return 0 if result['match'] else 1

if __name__=='__main__':
    sys.exit(main())
