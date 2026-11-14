"""The rule the stage-2 instruction models apply to RTC writes, and the decoding of the stage-2 progress words.

Stage 2 writes its progress to the RTC general-purpose words 0x07090100..0x0709011c (boot/a733/stage2/marks.h). It never
writes GP2 (0x07090108) through them; only a733_stage2_gate_entry() clears GP2, and the models track that clear apart from
the marks. It never writes the cold-boot marker (0x070901f0). docs/designs/card-boot-chain.md describes the words.
"""
RTC_WORDS = tuple(range(0x07090100, 0x07090120, 4))
GP2, MARKER = 0x07090108, 0x070901f0
TAG, LAST, SNAPSHOT, FAULT, ESR, ELR, FAR = (0x07090100, 0x07090104, 0x0709010c, 0x07090110, 0x07090114,
                                             0x07090118, 0x0709011c)
TAG_VALUE, STEP, FINISH, EXCEPTION, CPU = 0x53320001, 0x0c030000, 0x0c040000, 0x0c0e0000, 0x0c050000


def allowed(address, size):
    """True for a write that the progress words may make: one aligned 32-bit RTC general-purpose word other than GP2."""
    return size == 4 and address in RTC_WORDS and address != GP2


def negative_cases():
    """The rule refuses GP2, the marker, a word past FAR, an unaligned word and a narrow write."""
    for address, size in ((GP2, 4), (MARKER, 4), (0x07090120, 4), (0x07090102, 4), (LAST, 2)):
        assert not allowed(address, size), hex(address)
    return ['rule-refuses-gp2', 'rule-refuses-marker', 'rule-refuses-past-far', 'rule-refuses-unaligned',
            'rule-refuses-narrow']


def step(last):
    """The step and its detail from the LAST word, or None when it is not a stage-2 mark."""
    return (last >> 8 & 0xff, last & 0xff) if last >> 16 == STEP >> 16 else None
