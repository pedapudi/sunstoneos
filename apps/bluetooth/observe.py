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
    if any(frame[148:160]):
        raise ValueError('Capture reserved bytes differ.')
    result={'state':['pending','captured','retained-failure'][state]}
    if state!=1:
        if length or any(frame[64:148]):
            raise ValueError('Incomplete capture published producer metadata.')
        return result,b''
    firmware,uart,acl,audio=struct.unpack_from('<4Q',frame,64)
    total,records,loss,actual_kind,actual_offset=struct.unpack_from('<5I',frame,96)
    expected=struct.unpack_from('<I',profile,720)[0]*2
    extent=records*24 if kind else total
    if not firmware or not uart or firmware==uart or not acl or not audio or total!=expected or not 0<records<=1024 or loss or (actual_kind,actual_offset,length)!=(kind,offset,count) or offset+length>extent:
        raise ValueError('Capture identity, loss, or requested range differs.')
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
