#!/usr/bin/env python3
"""Copy compute output and explicitly dispose/reopen an owned GPU session."""
import functools
import math
import struct
import time

STATUS,READ,SUBMIT,CLOSE,DISPOSE,OPEN=range(6)
MAX_EPOCH=2**64-1


def operation(function):
    @functools.wraps(function)
    def run(self,*args,**kwargs):
        self._usable()
        try:
            result=function(self,*args,**kwargs)
            self.remaining()
            return result
        except BaseException as error:
            self.failure={'error':repr(error),'identity':self.identity(),
                          'last_request':self.last_request,'last_response':self.last_response,
                          'observed':self.last_receipt,'partial':bytes(self.partial)}
            raise
    return run


class Session:
    """GPU-only lifetime bookkeeping; retain(kind, identity, bytes) is durable.

    The coordinator supplies its original absolute monotonic deadline and an
    already validated/locked transport. Callback return confirms persistence.
    A failure poisons this instance; its observed receipt and partial copy stay
    available without making another transport request.
    """
    def __init__(self,epoch,deadline,retain,profile):
        if type(epoch) is not int or not 0<epoch<=MAX_EPOCH or not math.isfinite(deadline) or deadline<=time.monotonic() or not callable(retain):
            raise ValueError('GPU lifecycle identity, deadline or durable callback')
        self.session_epoch=epoch;self.deadline=deadline;self.retain=retain;self.profile=profile
        self.queue_epoch=0;self.sequence=0;self.state=None;self.copied=None
        self.failure=None;self.last_request=b'';self.last_response=b'';self.last_receipt=None
        self.partial=bytearray();self.receipts=[]
        self.outputs=[];self.closed_durable=False;self.disposed_durable=False

    def _usable(self):
        if self.failure is not None:raise RuntimeError('GPU lifecycle stopped after uncertainty')

    def remaining(self):
        remaining=self.deadline-time.monotonic()
        if remaining<=0:raise TimeoutError('GPU lifecycle absolute deadline')
        return remaining

    def identity(self):
        return {'profile':self.profile,'session_epoch':self.session_epoch,
                'queue_epoch':self.queue_epoch,'sequence':self.sequence,'status':self.state}

    def _persist(self,kind,payload):
        identity=self.identity();payload=bytes(payload)
        # Preserve the observed value even when publication itself fails.
        self.receipts.append({'kind':kind,'identity':identity,'bytes':payload})
        self.retain(kind,dict(identity),payload)

    def _accept(self,reply,*,epochs=None):
        allowed=(self.queue_epoch,) if epochs is None else epochs
        if reply['queue_epoch'] not in allowed or not reply['queue_epoch']:
            raise ValueError('GPU queue epoch differs')
        if reply['status'] in (2,4):raise ValueError('GPU retained failure or refused request')
        return reply

    def _observed(self,reply):
        self.queue_epoch=reply['queue_epoch'];self.sequence=reply['sequence'];self.state=reply['status']

    def _status(self,*,opening=False):
        old=self.queue_epoch
        reply=self._call(STATUS,0,queue_epoch=0 if not old or opening else old)
        if not old:
            if not reply['queue_epoch'] and (reply['status']!=0 or reply['sequence']):
                raise ValueError('GPU discovery returned invalid zero queue epoch')
        elif opening:
            self._accept(reply,epochs=(old,old+1))
        else:self._accept(reply)
        if reply['status'] in (2,4):raise ValueError('GPU discovery failed or refused')
        return reply

    @operation
    def status(self):
        reply=self._status()
        if self.queue_epoch and reply['sequence']!=self.sequence and not (self.state==0 and self.sequence==0 and reply['sequence']==self.initial_sequence):
            raise ValueError('GPU STATUS sequence changed unexpectedly')
        self._observed(reply);return dict(reply)

    def _ready(self):
        if self.state is None:self._observed(self._status())
        while self.state==0:
            time.sleep(.001);self._observed(self._status())

    def _protect_output(self):
        if self.sequence and self.copied!=(self.queue_epoch,self.sequence):
            raise ValueError('Copy the current GPU output before invalidating it')

    @operation
    def close(self):
        self._ready()
        if self.state!=1:raise ValueError('GPU CLOSE requires a ready queue')
        self._protect_output();old=self.sequence
        reply=self._accept(self._call(self.close_op,old))
        while reply['status']==0:
            if reply['sequence']!=old:raise ValueError('GPU CLOSE sequence changed')
            time.sleep(.001);reply=self._status()
        if reply['status']!=3 or reply['sequence']!=old:raise ValueError('GPU CLOSE incomplete')
        self._observed(reply);self._persist('closed',self.last_response);self.closed_durable=True
        return dict(reply)

    @operation
    def dispose(self):
        if self.state!=3 or not self.closed_durable:raise ValueError('GPU DISPOSE requires durable CLOSED')
        old=self.sequence;reply=self._accept(self._call(self.dispose_op,old))
        while reply['status']==0:
            if reply['sequence']!=old:raise ValueError('GPU DISPOSE sequence changed')
            time.sleep(.001);reply=self._status()
        if reply['status']!=5 or reply['sequence']!=old:raise ValueError('GPU DISPOSE incomplete')
        self._observed(reply);self._persist('disposed',self.last_response);self.disposed_durable=True
        return dict(reply)

    @operation
    def open(self):
        if self.state!=5 or not self.disposed_durable or self.queue_epoch==MAX_EPOCH:
            raise ValueError('GPU OPEN requires durable DISPOSED without epoch wrap')
        old=self.queue_epoch;previous=self.sequence
        reply=self._call(self.open_op,previous)
        while reply['status']==0:
            self._accept(reply,epochs=(old,))
            if reply['sequence']!=previous:raise ValueError('GPU pending OPEN sequence changed')
            time.sleep(.001);reply=self._status(opening=True)
        self._accept(reply,epochs=(old+1,))
        if reply['status']!=1 or reply['sequence']!=self.initial_sequence:
            raise ValueError('GPU OPEN did not publish a fresh queue')
        self._observed(reply);self.copied=None;self.closed_durable=False;self.disposed_durable=False
        self._persist('opened',self.last_response)
        return dict(reply)


class Compute(Session):
    """Version-6 compute consumer; CLOSE retains unless dispose/open are called."""
    version=6;page=32;close_op=CLOSE;dispose_op=DISPOSE;open_op=OPEN;initial_sequence=1
    maximum_sequence=2**32-1;maximum_words=992
    def __init__(self,exchange,epoch,words,*,deadline,retain):
        if not callable(exchange) or type(words) is not int or not 0<words<=self.maximum_words:
            raise ValueError('GPU compute exchange or output extent')
        super().__init__(epoch,deadline,retain,'compute');self.exchange=exchange;self.total=words

    def _call(self,op,seq,first=0,payload=b'',queue_epoch=None):
        q=self.queue_epoch if queue_epoch is None else queue_epoch
        request=struct.pack('<4IQ2IQ',0x49525047,self.version,op,seq,self.session_epoch,first,len(payload),q)+payload
        self.last_request=request;self.last_receipt=None
        raw=bytes(self.exchange(request,self.remaining()));self.last_response=raw
        if len(raw)<48:raise ValueError('Short GPU lifecycle response')
        magic,version,state,index,count,total,sequence,lo,hi,acquired=struct.unpack_from('<10I',raw)
        returned_epoch=struct.unpack_from('<Q',raw,40)[0]
        if (magic,version,index,total,lo|hi<<32)!=(0x50555047,self.version,first//self.page,self.total,self.session_epoch):
            raise ValueError('GPU lifecycle response identity')
        if state not in range(6) or sequence>self.maximum_sequence or len(raw)!=48+4*count:
            raise ValueError('GPU lifecycle response extent')
        expected=min(self.page,self.total-first)
        if op==READ and state==1:
            if count!=expected or expected<=0 or acquired!=self.total:raise ValueError('Incomplete GPU copied page')
        elif count or acquired:raise ValueError('Unexpected GPU lifecycle payload')
        reply={'status':state,'sequence':sequence,'queue_epoch':returned_epoch,'data':raw[48:]}
        self.last_receipt=reply;return reply

    @classmethod
    def for_device(cls,device,epoch,words,*,deadline,retain):
        def exchange(request,remaining):
            if device.write(0x02,request,timeout=max(1,int(remaining*1000)))!=len(request):
                raise ValueError('Short GPU lifecycle request')
            remaining=deadline-time.monotonic()
            if remaining<=0:raise TimeoutError('GPU lifecycle deadline after write')
            return device.read(0x81,48+cls.page*4,timeout=max(1,int(remaining*1000)))
        return cls(exchange,epoch,words,deadline=deadline,retain=retain)

    def _copy(self,current,previous):
        self.partial=bytearray()
        for first in range(0,self.total,self.page):
            while True:
                reply=self._accept(self._call(READ,current,first))
                if reply['status']==1:
                    if reply['sequence']!=current:raise ValueError('GPU copied sequence changed')
                    self.partial.extend(reply['data']);break
                if reply['status']!=0 or reply['sequence'] not in (previous,current):
                    raise ValueError('GPU queue closed before output')
                time.sleep(.001)
        self.sequence=current;self.state=1;result=bytes(self.partial)
        self.outputs.append({'identity':self.identity(),'bytes':result})
        self._persist('output',result);self.copied=(self.queue_epoch,current);self.partial.clear()
        return result

    @operation
    def read(self):
        self._ready()
        if self.state!=1 or not self.sequence:raise ValueError('GPU has no copied output')
        return self._copy(self.sequence,self.sequence)

    def _input(self,data):
        if not 4<=len(data)<=4096 or len(data)%4:raise ValueError('GPU compute input bounds')

    @operation
    def submit(self,data):
        data=bytes(data);self._input(data)
        self._ready()
        if self.state!=1 or self.sequence>=self.maximum_sequence:raise ValueError('GPU submission state or sequence exhausted')
        self._protect_output();previous=self.sequence
        reply=self._accept(self._call(SUBMIT,previous,payload=data))
        if reply['status']!=0 or reply['sequence']!=previous:raise ValueError('GPU submission not queued')
        self.state=0;return self._copy(previous+1,previous)
