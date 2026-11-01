"""Decode the empty TYPEC24 diagnostic; electrical voltage is not inferred."""
import struct
FIELDS = ('magic mode failure state rose fell rise_us fall_us max_gap cc_samples '
          'guard_gap guard_fault on_us pl8_cfg pl8_dat twi_state twi_status rate_hz divider bus_retired').split()
def decode(payload):
    if len(payload) != 8*len(FIELDS):
        raise ValueError('Unexpected VBUS report length')
    result = dict(zip(FIELDS, struct.unpack('<20Q', payload)))
    if result['magic'] != 0x3154505256425353 or result['mode'] not in (0, 1, 10):
        raise ValueError('Unexpected VBUS report identity')
    return result
def failures(result):
    errors=[]
    for field in ('failure','guard_fault'):
        if result[field]: errors.append(f'{field}={result[field]:#x}')
    if result['cc_samples'] < 2: errors.append('Both CC pins were not observed open')
    if result['bus_retired'] != 1 or result['twi_state'] != 1 or result['twi_status'] != 0xf8:
        errors.append('I2C retirement is incomplete')
    if (result['rate_hz'],result['divider']) not in ((400000,40),(384000,32),(371428,48)):
        errors.append('Unexpected source-derived 400kHz divider')
    if not result['fell']: errors.append('VBUSOK did not fall')
    if result['pl8_dat'] & 0x100: errors.append('PL8 remains high')
    if result['pl8_cfg'] & 15 != 1: errors.append('PL8 is not GPIO output')
    if result['max_gap'] > 2000 or result['guard_gap'] > 2000: errors.append('Service gap exceeds 2ms')
    if result['mode']:
        if result['rise_us'] >= 275000: errors.append('VBUSOK rise exceeds 275ms')
        if not result['rose']: errors.append('VBUSOK did not rise')
        if not result['mode']*1000000-20000 <= result['on_us'] <= result['mode']*1000000: errors.append('Source duration exceeds budget')
    elif result['on_us'] or result['rose'] or result['rise_us']: errors.append('Control enabled source')
    if result['fall_us'] >= 650000: errors.append('VBUSOK fall exceeds 650ms')
    return errors
