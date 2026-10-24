"""Bounded returned-RAM capture for the audited numerical image only.

Every read uses immutable image-bound addresses. Captured pointers are checked
against them and never authorize a read. Payload differences remain diagnostic.
"""
import hashlib,struct
IMAGE='9e4adabf7c8fb19c2cfc2fb4260971cc32a3b6e5ee0fa09bcad0d4b4208c0e95'
PARENT=0x41239000;BYTES=0x109000;ARENA=PARENT+0x104000
METADATA=(('lengths-buffers',0x412156d8,368),('extent',0x4121f268,24),('memory',0x41216088,48))
SLOTS=((0,4096,0xe0000000,4),(0x1000,4096,0xe0002000,4),
       (0x2000,524288,0xe0004000,2),(0x82000,4096,0xe0085000,0),
       (0x83000,4096,0xe0087000,1),(0x84000,524288,0xe0089000,3))
def require(ok,message):
    if not ok:raise ValueError(message)
def sha(data):return hashlib.sha256(data).hexdigest()
def layout(image_hash,symbols,read,clean_start,clean_end):
    if image_hash!=IMAGE:return None
    require((clean_start,clean_end)==(0x41200000,0x41439000) and
        symbols['app']['st_value']==0x4120d300 and symbols['app']['st_size']==140096 and
        symbols['__device_pool_start']['st_value']==PARENT and symbols['__device_pool_end']['st_value']==clean_end,
        'Audited stopped-capture layout differs.')
    require(read('npu_convolution_layout')==b''.join(struct.pack('<QQII',*slot) for slot in SLOTS),
        'Audited native slot layout differs.')
    require((len(read('a733_npu_application_model')),len(read('a733_npu_application_input')),len(read('a733_npu_application_reference')))==(2056,432,400),
        'Audited convolution artifacts differ.')
    ranges=[(at,size) for _,at,size in METADATA]+[(PARENT,BYTES)]
    require(all(0x41000000<=clean_start<=at<at+size<=clean_end<=0x42200000 for at,size in ranges),'Capture escapes published DRAM.')
    return {'image_sha256':IMAGE,'metadata':[{'name':name,'address':at,'bytes':size} for name,at,size in METADATA],
        'parent':{'address':PARENT,'bytes':BYTES},'input_sha256':sha(read('a733_npu_application_input'))}
def validate_metadata(raw):
    header,extent,memory=(raw[name] for name,_,_ in METADATA)
    require(all(len(raw[name])==size for name,_,size in METADATA),'Short stopped metadata capture.')
    require(struct.unpack_from('<3Q',header)==(2056,432,400) and struct.unpack_from('<I',header,24)[0]==2,
        'Stopped model/tensor metadata differs.')
    require(struct.unpack_from('<2Q',header,352)==(136,208),'Stopped stream lengths differ.')
    for index,(offset,size,device,role) in enumerate(SLOTS):
        require(struct.unpack_from('<QQQII',header,32+32*index)==(PARENT+offset,PARENT+offset,size,device,role),
            'Stopped buffer geometry differs: '+str(index))
    require(struct.unpack('<3Q',extent)==(PARENT,PARENT,BYTES),'Stopped parent extent differs.')
    require(struct.unpack('<5Q2I',memory)==(6,ARENA,20480,ARENA,20480,(ARENA>>8)|1,1),'Stopped native memory image differs.')
def capture(transaction,base,p,evidence,record,save,result):
    plan=p['geometry'].get('stopped_capture')
    if plan is None or result['status']!=1:return
    require(p['execution_profile']=='npu-finite-return-v1' and p['image_sha256']==IMAGE and
        result['normal_return'] is True and result['controlled_negative'] is True and result['functional_success'] is False,
        'Stopped capture requires validated numerical-failure retirement.')
    record['stopped_capture']={'complete':False,'files':[]};save()
    def read(item):
        name=item['name'];address=item['address'];size=item['bytes'];path=evidence/('npu-stopped-'+name+'.bin')
        require(not path.exists(),'Stopped capture output already exists.')
        try:transaction('read',hex(address),size,path)
        finally:
            if path.is_file():
                base.sync_file(path);data=path.read_bytes()
                record['stopped_capture']['files'].append({'name':path.name,'address':address,'requested_bytes':size,'bytes':len(data),'sha256':sha(data)})
                save()
        require(path.is_file() and path.stat().st_size==size,'Short stopped capture: '+name)
        return data
    raw={item['name']:read(item) for item in plan['metadata']};validate_metadata(raw)
    record['stopped_capture']['metadata_validated']=True;save()
    parent=read({'name':'parent',**plan['parent']})
    record['stopped_capture'].update(complete=True,input_matches_immutable=sha(parent[0x82000:0x82000+432])==plan['input_sha256'],
        output_matches_returned_copy=parent[0x83000:0x83000+400]==bytes.fromhex(result['second_output']))
    save()
