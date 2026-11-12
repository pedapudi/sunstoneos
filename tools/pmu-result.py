#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Freeze an application decoder with the optional main-core PMU result prefix."""
import argparse
from pathlib import Path
import struct
import types

APPLICATION_SOURCE = None
APPLICATION_ORIGIN = None
PREFIX = struct.Struct('<6Q')
MAGIC = 0x313052554d505353
_application = None


def application():
    global _application
    if _application is None:
        if APPLICATION_SOURCE is None:
            raise ValueError('freeze this decoder with --decoder and --output first')
        _application = types.ModuleType('pmu_application_decoder')
        _application.__file__ = APPLICATION_ORIGIN
        exec(compile(APPLICATION_SOURCE, APPLICATION_ORIGIN, 'exec'), _application.__dict__)
    return _application


def inputs():
    app = application()
    return app.inputs() if hasattr(app, 'inputs') else []


def decode(payload):
    if len(payload) < PREFIX.size:
        raise ValueError('incomplete PMU result prefix')
    magic, core, cycles, instructions, data_walks, ticks = PREFIX.unpack_from(payload)
    if magic != MAGIC or core not in (2, 6):
        raise ValueError('PMU result identity differs')
    if not cycles or not instructions or not ticks or data_walks > 0xffffffff:
        raise ValueError('PMU result counts are missing or malformed')
    result = application().decode(payload[PREFIX.size:])
    if not isinstance(result, dict) or 'pmu' in result:
        raise ValueError('application decoder must return a dictionary without a pmu field')
    result['pmu'] = dict(scope='main_core_activity', core=core, cycles=cycles,
                         inst_retired=instructions, dtlb_walk_mod32=data_walks, counter_ticks=ticks)
    return result


def failures(result):
    app = application()
    app_result = {key: value for key, value in result.items() if key != 'pmu'}
    errors = list(app.failures(app_result)) if hasattr(app, 'failures') else []
    if 'matches_reference' in result and not result['matches_reference']:
        errors.append('application result differs from its reference')
    if hasattr(app, 'expected'):
        expected = app.expected(inputs())
        if any(result.get(key) != value for key, value in expected.items()):
            errors.append('application result differs from its expected input result')
    if not result.get('pmu', {}).get('inst_retired'):
        errors.append('retired-instruction count is missing')
    return errors


def freeze(decoder, output):
    source = Path(__file__).read_text()
    source = source.replace('APPLICATION_SOURCE = None', 'APPLICATION_SOURCE = ' + repr(Path(decoder).read_text()), 1)
    source = source.replace('APPLICATION_ORIGIN = None', 'APPLICATION_ORIGIN = ' + repr(str(Path(decoder).resolve())), 1)
    Path(output).write_text(source)


if APPLICATION_SOURCE is not None and hasattr(application(), 'BINDING'):
    BINDING = application().BINDING


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--decoder', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    freeze(args.decoder, args.output)
