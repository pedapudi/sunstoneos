#!/usr/bin/env python3
"""Decode a copied PWM register/IRQ/stop record; no physical waveform claim."""
import hashlib,struct
def decode(raw,session):
 if len(session)!=872 or struct.unpack_from('<3Q',session,592)!=(0x4153373350574131,2,872):raise ValueError('Wrong PWM session')
 if len(raw)!=512:raise ValueError('Incomplete PWM record')
 magic,version,status,count,epoch,generation=struct.unpack_from('<4I2Q',raw)
 if (magic,version)!=(0x314d5750,2) or status not in (0,1,2) or count!=512:raise ValueError('Wrong PWM framing')
 if (epoch,generation)!=struct.unpack_from('<2Q',session,24) or raw[32:64]!=session[48:80] or raw[64:96]!=hashlib.sha256(session).digest():raise ValueError('PWM session/build differs')
 names=('phase','error','started','irq_seen','pisr','pcr','ppr','counter','pulse_counter','waveform_stopped','gpio_low','bgr','apb0','voltage_mode','voltage_value','reserved')
 report=dict(zip(names,struct.unpack_from('<16I',raw,96)))
 started,completed=struct.unpack_from('<2Q',raw,160);plan=struct.unpack_from('<4IQ',raw,176)
 rail=struct.unpack_from('<I',raw,200)[0];enabled,decoded,uv=struct.unpack_from('<3I',raw,212)
 deadline=struct.unpack_from('<Q',raw,224)[0];initialized,fault,clock_retained,rail_retained,app_error,reserved=struct.unpack_from('<6I',raw,232)
 if report['reserved'] or reserved or any(raw[210:212]):raise ValueError('Nonzero PWM reserved field')
 seq,kind,arg,ack,pending,command_deadline,rejected,rejection,ready,step,led_level,completed_commands=struct.unpack_from('<Q4I2Q4IQ',raw,256)
 command_completed=struct.unpack_from('<Q',raw,344)[0]
 if any(raw[352:512]) or any(raw[330:332]) or raw[255]:raise ValueError('Nonzero service reserved field')
 if ack>2 or pending>1 or ready>1 or led_level>1 or kind>4 or step>6:raise ValueError('Invalid service state')
 if seq and (not 1<=seq<=struct.unpack_from('<I',session,864)[0] or not kind or
  (kind<=2 and not 1<=arg<600) or (kind==3 and arg) or (kind==4 and arg>1)):raise ValueError('Invalid command facts')
 if status==1:
  if (kind in (1,2) and report['phase']!=13) or (kind==3 and report['phase']!=16) or (kind==4 and led_level!=arg):raise ValueError('Command state differs')
  if not ready or pending or step or report['error'] or report['phase'] not in (13,16):raise ValueError('PWM service completion missing')
  if report['phase']==13 and (report['started'],report['irq_seen'],report['waveform_stopped'],report['gpio_low'])!=(1,1,0,0):raise ValueError('New start IRQ missing')
  if report['phase']==16 and (report['waveform_stopped'],report['gpio_low'])!=(1,1):raise ValueError('PWM stop facts missing')
  if plan[0]!=0x100 or plan[2]!=600 or not 1<=plan[3]<600 or plan[1]!=(599<<16|plan[3]) or plan[4]!=0:raise ValueError('Different cycle profile')
  if rail!=15 or raw[204]!=0 or raw[205]!=3 or raw[206]!=0 or raw[209]!=0 or (enabled,decoded,uv)!=(1,1,1800000):raise ValueError('PWM supply differs')
  if struct.unpack_from('<Q',session,784)[0] and (struct.unpack_from('<I',raw,320)[0]!=0 or raw[325]!=3 or struct.unpack_from('<3I',raw,332)!=(1,1,3300000)):raise ValueError('LED supply differs')
  if initialized!=1 or fault or app_error or not completed<deadline or (seq and (ack!=2 or not 0<command_completed<command_deadline)):raise ValueError('PWM lifetime/state differs')
 return dict(status=status,epoch=epoch,generation=generation,report=report,
  command=dict(sequence=seq,kind=kind,argument=arg,ack_state=ack,pending=pending,deadline_us=command_deadline),
  rejected_sequence=rejected,rejection=rejection,ready=ready,service_step=step,led_level=led_level,completed_commands=completed_commands,command_completed_us=command_completed,started_us=started,completed_us=completed,
  plan=dict(pcr=plan[0],ppr=plan[1],ticks=plan[2],active=plan[3],duration_us=plan[4]),
  supply=dict(rail=rail,page_before=raw[204],chip=raw[205],revision=raw[206],enable=raw[207],selector=raw[208],page_after=raw[209],enabled=enabled,decoded=decoded,programmed_uv=uv),
  deadline_us=deadline,initialized=initialized,fault=fault,clock_retained=clock_retained,rail_retained=rail_retained,application_error=app_error,
  physical_waveform='not established by register evidence',physical_fan_off='requires gate/current measurement')

def command(session,sequence,kind,argument=0,budget_us=100000):
 if len(session)!=872 or not 1<=sequence<=struct.unpack_from('<I',session,864)[0] or kind not in (1,2,3,4) or not 20000<=budget_us<=100000:raise ValueError('Invalid command envelope')
 if (kind<=2 and not 1<=argument<600) or (kind==3 and argument) or (kind==4 and (argument not in (0,1) or not struct.unpack_from('<Q',session,784)[0])):raise ValueError('Invalid fixed PWM request')
 epoch,generation=struct.unpack_from('<2Q',session,24)
 return b'\2'+struct.pack('<2I3Q4I32s',0x324d5750,2,epoch,generation,sequence,kind,argument,budget_us,0,hashlib.sha256(session).digest())
