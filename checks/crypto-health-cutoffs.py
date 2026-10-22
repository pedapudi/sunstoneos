#!/usr/bin/env python3
"""Derive CE TRNG health-test register words from an assessed entropy per raw sample.

NIST SP 800-90B section 4.4 defines the two continuous health tests that the
A733 crypto engine implements in hardware:

  repetition count:    C = 1 + ceil(-log2(alpha) / H)
  adaptive proportion: C = 1 + CRITBINOM(W, 2^-H, 1 - alpha)

H is the assessed min-entropy per raw noise sample, alpha = 2^-k is the
false-positive probability (20 <= k <= 40) and W is the window: 1024 samples
for a binary source and 512 otherwise. CRITBINOM(n, p, q) is the smallest c
with P(X <= c) >= q for X ~ Binomial(n, p).

With only --output, the script checks its arithmetic against the cutoffs
published in SP 800-90B section 4.4 and writes them as JSON. With --entropy it
also prints the four secure register words in the form a733_ce_init accepts:
0x90 repetition cutoff, 0x94 adaptive cutoff, 0x98 zero (continuous testing)
and 0x9c both enables with the window. The result is only as good as H: it
must come from a raw-source assessment of this silicon, which does not exist
yet. The script supplies no default and never selects H itself.
"""
import argparse
from decimal import Decimal, getcontext
from fractions import Fraction
import json
import math
from pathlib import Path

getcontext().prec = 80


def repetition(entropy, k):
    return 1 + math.ceil(Fraction(k) / entropy)


def adaptive(entropy, k, window):
    """1 + CRITBINOM(W, 2^-H, 1 - 2^-k), from the exact upper tail."""
    p = Decimal(2) ** (-Decimal(entropy.numerator) / Decimal(entropy.denominator))
    alpha = Decimal(2) ** -k
    # Smallest c with P(X > c) <= alpha; accumulate the tail from the top.
    tail = Decimal(0)
    for c in range(window, -1, -1):
        # tail is P(X > c) on entry.
        if tail > alpha:
            return 1 + c + 1
        tail += math.comb(window, c) * p ** c * (1 - p) ** (window - c)
    return 1


def words(entropy, k, binary):
    window = 1024 if binary else 512
    return [repetition(entropy, k), adaptive(entropy, k, window), 0, 0x30000 | window]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--entropy', type=Fraction, help='assessed min-entropy bits per raw sample, e.g. 0.8 or 4/5')
    ap.add_argument('--alpha-log2', type=int, default=20, help='k for alpha = 2^-k, 20..40')
    ap.add_argument('--binary', action='store_true', help='the raw noise source is binary (W = 1024)')
    a = ap.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    # SP 800-90B 4.4.1: alpha = 2^-20 and H = 4 give a repetition cutoff of 6.
    assert repetition(Fraction(4), 20) == 6
    # SP 800-90B 4.4.2 Table 2, alpha = 2^-20: binary W = 1024, non-binary W = 512.
    published = {(Fraction(1, 5), True): 941, (Fraction(2, 5), True): 840,
                 (Fraction(3, 5), True): 748, (Fraction(4, 5), True): 664, (Fraction(1), True): 589,
                 (Fraction(1, 2), False): 410, (Fraction(1), False): 311, (Fraction(2), False): 177,
                 (Fraction(4), False): 62, (Fraction(8), False): 13}
    computed = {}
    for (entropy, binary), cutoff in published.items():
        value = adaptive(entropy, 20, 1024 if binary else 512)
        computed['%s-%s' % (entropy, 'binary' if binary else 'non-binary')] = value
        assert value == cutoff, (entropy, binary, value, cutoff)
    record = {'published_adaptive_cutoffs': computed}
    if a.entropy is not None:
        assert 0 < a.entropy and (a.entropy <= 1 or not a.binary), 'a binary sample carries at most one bit'
        assert 20 <= a.alpha_log2 <= 40, 'SP 800-90B recommends 2^-40 <= alpha <= 2^-20'
        result = words(a.entropy, a.alpha_log2, a.binary)
        assert 2 <= result[0] and result[1] <= (result[3] & 2047)
        record['request'] = {'entropy': str(a.entropy), 'alpha_log2': a.alpha_log2, 'binary': a.binary,
                             'health': ['0x%08x' % w for w in result]}
        print('health = {%s}' % ', '.join('0x%x' % w for w in result))
    (a.output / 'result.json').write_text(json.dumps(record, indent=1) + '\n')
    print('PASS SP 800-90B cutoff arithmetic matches the published table; no entropy assessment')


if __name__ == '__main__':
    main()
