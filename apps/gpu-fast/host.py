"""Decode GPU empty-job measurements in architectural-counter ticks."""
import struct


def decode(payload):
    if len(payload)!=96:
        raise ValueError('GPU benchmark needs its complete 96-byte result')
    w=struct.unpack('<24I',payload)
    magic,version,cache,jobs,frequency,serial,pipeline,irq=w[:8]
    if magic!=0x31464750 or version not in (1,2) or cache>1 or jobs not in (0,10000) or frequency!=24000000:
        raise ValueError('GPU benchmark identity, mode, sample count or counter differs')
    wake_samples=irq>>16 if version==2 else 0
    irq=irq&65535 if version==2 else irq
    if (serial,pipeline)!=(jobs,jobs) or irq>jobs or wake_samples>irq or w[19]!=(4 if jobs else 0) or w[20]!=(64 if jobs else 0) or (version==1 and any(w[21:])):
        raise ValueError('GPU benchmark completion, depth, warmup or reserved fields differ')
    if not jobs and any(w[8:]):
        raise ValueError('Zero-job control contains measurements')
    for values in (w[8:11],w[11:14],w[14:17],w[21:24]):
        if list(values)!=sorted(values) or (jobs and values[-1]>=frequency//5):
            raise ValueError('GPU benchmark distribution is unordered or exceeds the job deadline')
    us=lambda values:dict(zip(('p50','p99','maximum'),[round(v*1e6/frequency,3) for v in values]))
    throughput=round(jobs*frequency/w[17],3) if jobs and w[17] else 0
    return dict(payload_cache=('writeback','uncached')[cache],jobs=jobs,serial_jobs=serial,pipeline_jobs=pipeline,
                irq_samples=irq,wake_samples=wake_samples,rtt_us=us(w[8:11]),submit_wall_us=us(w[11:14]),
                irq_to_wait_return_us=us(w[14:17]),irq_to_notification_return_us=us(w[21:24]),
                pipeline_jobs_per_second=throughput,pipeline_submit_wall_us=round(w[18]*1e6/frequency,3),
                software_depth=w[19],warmup_jobs=w[20],
                targets_met=bool(jobs and w[9]*1e6/frequency<=50 and w[13]*1e6/frequency<=5 and
                                 wake_samples==jobs and w[23]*1e6/frequency<=20 and throughput>=10000))
