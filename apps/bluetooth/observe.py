#!/usr/bin/env python3
"""Decode copied HCI identity without granting security or connection readiness."""
import hashlib
import struct


def decode(frame, session, profile):
    if len(session) != 592 or struct.unpack_from('<3Q', session) != (0x4153373342535332, 2, 592):
        raise ValueError('Supervised board session required.')
    if struct.unpack_from('<I', session, 400)[0] != 2:
        raise ValueError('USB0-only shared startup required.')
    if len(frame) < 80:
        raise ValueError('Short Bluetooth frame.')
    magic, version, state, length, epoch, generation = struct.unpack_from('<4I2Q', frame)
    if (magic, version) != (0x43485442, 1) or state > 2:
        raise ValueError('Unsupported Bluetooth result.')
    if (epoch, generation) != struct.unpack_from('<2Q', session, 24):
        raise ValueError('Session identity differs.')
    if hashlib.sha256(profile).digest() != session[320:352] or frame[32:64] != session[320:352]:
        raise ValueError('Authenticated Bluetooth profile differs.')
    if len(frame) != 80 + length or length != (97 if state == 1 else 0):
        raise ValueError('Identity extent differs.')
    firmware, uart = struct.unpack_from('<2Q', frame, 64)
    result = {'state': ['pending', 'identified', 'retained-failure'][state],
              'epoch': epoch, 'generation': generation}
    if state == 1:
        if not firmware or not uart or firmware == uart or struct.unpack_from('<I', frame, 80)[0] != 63:
            raise ValueError('Incomplete actual controller identity.')
        result.update(firmware_generation=firmware, uart_generation=uart,
                      version=frame[84:92].hex(), supported_commands=frame[92:156].hex(),
                      features=frame[156:164].hex(), buffer_size=frame[164:171].hex(), address=frame[171:177].hex())
    elif firmware or uart:
        raise ValueError('Pending/failed frame published controller generations.')
    return result


def decode_pcm(frame,session,profile,kind,offset,count):
    """Decode one immutable post-disconnect copy; caller verifies the full hash."""
    if len(profile) not in (752,832) or struct.unpack_from('<II',profile,8)!=({752:2,832:3}.get(len(profile),0),len(profile)):
        raise ValueError('Headset capture profile required.')
    if len(session)!=592 or struct.unpack_from('<3Q',session)!=(0x4153373342535332,2,592):
        raise ValueError('Supervised board session required.')
    if kind not in (0,1) or not 0<count<=1024 or offset<0 or len(frame)<160:
        raise ValueError('Invalid copied capture request or extent.')
    magic,version,state,length,epoch,generation=struct.unpack_from('<4I2Q',frame)
    if (magic,version)!=(0x4d435442,1) or state>2 or len(frame)!=160+length:
        raise ValueError('Invalid copied capture frame.')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or frame[32:64]!=session[320:352] or hashlib.sha256(profile).digest()!=session[320:352]:
        raise ValueError('Capture session/profile identity differs.')
    a2dp=struct.unpack_from('<I',profile,624)[0]==6
    if any(frame[148:160]) and not a2dp:
        raise ValueError('Capture reserved bytes differ.')
    result={'state':['pending','captured','retained-failure'][state]}
    if state!=1:
        if length or any(frame[64:160]):
            raise ValueError('Incomplete capture published producer metadata.')
        return result,b''
    firmware,uart,acl,audio=struct.unpack_from('<4Q',frame,64)
    total,records,loss,actual_kind,actual_offset=struct.unpack_from('<5I',frame,96)
    expected=struct.unpack_from('<I',profile,720)[0]*2
    extent=records*24 if kind else total
    if not firmware or not uart or firmware==uart or not acl or not audio or total!=expected or not 0<records<=(1500 if a2dp else 1024) or loss or (actual_kind,actual_offset,length)!=(kind,offset,count) or offset+length>extent:
        raise ValueError('Capture identity, loss, or requested range differs.')
    if a2dp:
        rate,channels,codec=struct.unpack_from('<3I',frame,148)
        if rate not in (44100,48000) or channels not in (1,2) or codec!=1 or total%(channels*2):
            raise ValueError('Invalid A2DP PCM geometry.')
        result.update(sample_rate=rate,channels=channels,codec='SBC',sample_format='s16le')
    result.update(firmware_generation=firmware,uart_generation=uart,acl_generation=acl,audio_generation=audio,
                  bytes=total,records=records,loss_packets=loss,sha256=frame[116:148].hex())
    return result,frame[160:]


def decode_recognition(frame,session,profile,offset,count):
    if len(profile)!=832 or struct.unpack_from('<II',profile,8)!=(3,832) or len(frame)<208:
        raise ValueError('Finite headset recognition profile/frame required.')
    if len(session)!=592 or struct.unpack_from('<3Q',session)!=(0x4153373342535332,2,592) or struct.unpack_from('<I',session,400)[0]!=2:
        raise ValueError('Supervised Bluetooth session required.')
    magic,version,state,length,epoch,generation=struct.unpack_from('<4I2Q',frame)
    if (magic,version)!=(0x52544242,1) or state>2 or len(frame)!=208+length or not 0<count<=1024:
        raise ValueError('Recognition frame geometry differs.')
    if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or frame[32:64]!=session[320:352] or hashlib.sha256(profile).digest()!=session[320:352]:
        raise ValueError('Recognition session/profile identity differs.')
    result={'state':['pending','recognized','retained-failure'][state]}
    if state!=1:
        if length or any(frame[64:208]):raise ValueError('Pending/failed recognition published producer metadata.')
        return result,b''
    firmware,uart,acl,audio=struct.unpack_from('<4Q',frame,64)
    total,source,converted,reserved=struct.unpack_from('<4I',frame,96)
    if not firmware or not uart or firmware==uart or not acl or not audio or not 0<total<=65536 or source!=struct.unpack_from('<I',profile,720)[0] or converted!=2*source or reserved or offset<0 or length!=count or offset+count>total:
        raise ValueError('Recognition generations, sample counts or copied extent differ.')
    result.update(firmware_generation=firmware,uart_generation=uart,acl_generation=acl,audio_generation=audio,
                  bytes=total,input_samples=source,output_samples=converted,pcm_sha256=frame[112:144].hex(),
                  wav_sha256=frame[144:176].hex(),text_sha256=frame[176:208].hex())
    return result,frame[208:]

def decode_repeated(frame,session,profile,offset=0):
    """Decode one copied mode2 transcript/status; no physical-disconnect inference.

    A recognition activation ends early at the headset's AT+BVRA=0, so a
    segment holds up to the profile's samples and first_sample counts the
    samples of earlier segments. One frame bounds first_sample by the ticket;
    the caller, which sees every ticket, checks that it is contiguous."""
    def require(ok,message):
        if not ok:raise ValueError(message)
    mode=struct.unpack_from('<I',profile,624)[0] if len(profile)==832 else 0
    require(mode in (2,3,5),'Repeated headset profile required.')
    require(272<=len(frame)<=1296 and struct.unpack_from('<II',frame)==(0x53525442,2 if mode in (3,5) else 1),'Repeated Bluetooth frame/version differs.')
    state,count=struct.unpack_from('<2I',frame,8)
    require(state<=3 and count<=1024 and len(frame)==272+count and
            struct.unpack_from('<2Q',frame,16)==struct.unpack_from('<2Q',session,24) and
            frame[32:64]==hashlib.sha256(profile).digest(),'Repeated Bluetooth identity or extent differs.')
    firmware,uart,acl,audio,ticket,first=struct.unpack_from('<6Q',frame,64)
    total,inputs,outputs,reserved=struct.unpack_from('<4I',frame,112)
    published,consumed=struct.unpack_from('<2Q',frame,224)
    flags,disconnected=struct.unpack_from('<2I',frame,240)
    first_acquired,last_acquired=struct.unpack_from('<2Q',frame,248)
    finished,returned=struct.unpack_from('<2I',frame,264)
    require((mode in (3,5) or not reserved) and flags<=31 and disconnected<=1 and finished<=1 and consumed<=published and
            (not finished or disconnected) and returned==offset,'Repeated Bluetooth status or reserved field differs.')
    if ticket:
        codec=reserved if mode in (3,5) else 1
        require(codec in (1,2),'Repeated Bluetooth codec differs.')
        samples=struct.unpack_from('<I',profile,720)[0]
        require(all((firmware,uart,acl,audio)) and ticket<=consumed and
                first<=(ticket-1)*samples and 0<inputs<=samples and outputs==(inputs if codec==2 else 2*inputs) and
                total<=65536 and first_acquired<=last_acquired and first_acquired and
                offset<=total and count<=total-offset and state in (1,3),'Repeated transcript metadata differs.')
        require(all(any(frame[n:n+32]) for n in (128,160,192)),'Repeated transcript digests missing.')
    else:require(not count and not any(frame[64:224]) and not any(frame[248:264]),'Unpublished transcript metadata leaked.')
    return {'codec':reserved if mode in (3,5) and ticket else 1 if ticket else 0,'sample_rate':16000 if reserved==2 else 8000 if ticket else 0,'state':state,'firmware':firmware,'uart':uart,'acl':acl,'audio':audio,'ticket':ticket,'first_sample':first,
            'bytes':total,'input_samples':inputs,'output_samples':outputs,'pcm_sha256':frame[128:160].hex(),
            'wav_sha256':frame[160:192].hex(),'text_sha256':frame[192:224].hex(),'published':published,'consumed':consumed,
            'flags':flags,'disconnected':disconnected,'finished':finished,'first_acquired':first_acquired,'last_acquired':last_acquired},frame[272:]


def decode_playback(frame,session,profile):
    """Decode copied caller-PCM accounting; controller release is not rendering."""
    def require(ok,message):
        if not ok:raise ValueError(message)
    require(len(profile)>=752 and struct.unpack_from('<I',profile,624)[0] in (4,5),'Caller playback profile required.')
    require(len(frame)==332 and struct.unpack_from('<2I',frame)==(0x50505442,1),'Playback response geometry differs.')
    state,outcome=struct.unpack_from('<2I',frame,8)
    require(state<=2 and outcome in (1,2,3),'Playback response state differs.')
    require(struct.unpack_from('<2Q',frame,16)==struct.unpack_from('<2Q',session,24) and frame[32:64]==session[320:352] and frame[32:64]==hashlib.sha256(profile).digest(), 'Playback session identity differs.')
    # Session offsets follow the existing board session, not the response layout.
    firmware,uart,acl,audio,ticket,encoded,submitted,uart_packets,released,flushed,not_sent=struct.unpack_from('<11Q',frame,64)
    flags,codec,rate,period=struct.unpack_from('<4I',frame,184)
    accepted,acked,silence,discarded_packets,discarded_bytes,loss=struct.unpack_from('<6Q',frame,200)
    sealed,cancelled,closed,free=struct.unpack_from('<4I',frame,248)
    silence_uart,silence_released,silence_flushed,silence_not_sent=struct.unpack_from('<4Q',frame,264)
    op=struct.unpack_from('<I',frame,328)[0]
    require(rate==16000 and period==2400 and flags<=3 and codec in (0,2) and op<=5 and sealed<=1 and cancelled<=1 and closed<=1 and acked<=accepted and accepted-acked<=2 and ((state==2 and not any(frame[200:296])) or (state!=2 and free==2-(accepted-acked))),'Playback counters/format differ.')
    if state==1:
        require(all((firmware,uart,acl,audio,ticket)) and codec==2 and ticket==acked+1 and encoded<=1200 and encoded%120==0 and submitted<=encoded//120*60 and released+flushed==uart_packets,'Playback period ownership differs.')
        require(flags or (encoded==1200 and submitted==600 and not flushed and not not_sent),'Incomplete PCM reported successful.')
    else:require(not any(frame[64:192]),'Pending/fault response exposed mutable period fields.')
    return dict(state=state,outcome=outcome,op=op,firmware=firmware,uart=uart,acl=acl,audio=audio,ticket=ticket,encoded_samples=encoded,submitted_bytes=submitted,uart_packets=uart_packets,released_packets=released,flushed_packets=flushed,not_sent_packets=not_sent,sha256=frame[152:184].hex(),flags=flags,codec=codec,rate=rate,period_bytes=period,accepted=accepted,acknowledged=acked,silence_frames=silence,discarded_packets=discarded_packets,discarded_bytes=discarded_bytes,loss_packets=loss,sealed=sealed,cancelled=cancelled,disconnected=closed,free_slots=free,silence_uart=silence_uart,silence_released=silence_released,silence_flushed=silence_flushed,silence_not_sent=silence_not_sent,build_id=frame[296:328].hex())
