#!/usr/bin/env python3
"""Focused parser, caching, read-only queue and HTTP boundary checks."""
import hashlib
import http.client
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import tempfile
import threading
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('progress', ROOT / 'tools/progress.py')
progress = importlib.util.module_from_spec(spec)
spec.loader.exec_module(progress)


class Check(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'docs').mkdir()
        for name in progress.SOURCES:
            (self.root / name).write_bytes((ROOT / name).read_bytes())
        self.queue = self.root / 'queue.sqlite3'
        self.view = progress.Progress(self.root, {'3-1': self.queue})

    def test_inventory_and_evidence(self):
        rows, parser = progress.parse_hardware((ROOT / 'docs/hardware.md').read_text())
        inventory = [line for line in (ROOT / 'docs/hardware.md').read_text().splitlines()
                     if line.startswith('| ') and not line.startswith(('| Hardware', '| ---'))]
        self.assertEqual(len(rows), len(inventory))
        self.assertEqual(parser['status'], 'ok')
        by_name = {row['name']: row for row in rows}
        self.assertNotIn('board', by_name['DMA and device address translation']['evidence'])
        self.assertIn('host', by_name['DMA and device address translation']['evidence'])
        self.assertIn('board', by_name['CPU cores']['evidence'])
        self.assertTrue(all(row['qualification'] == 'unqualified' for row in rows))
        self.assertTrue(by_name['GPU']['details'][0]['markdown'])
        # Each physical anchor still matches its own live bullet, including DDR and RTC.
        self.assertFalse([w for w in parser['warnings'] if 'Physical-result anchor' in w], parser['warnings'])
        self.assertIn('ddr', progress.PHYSICAL_SCOPES)
        self.assertIn('rtc and watchdog', progress.PHYSICAL_SCOPES)
        for name in ('DDR and CPU memory management', 'RTC and watchdog'):
            self.assertIn('board', by_name[name]['evidence'])
        reworded = (ROOT / 'docs/hardware.md').read_text().replace(
            'coherent RTC reads in inherited normal RC mode', 'RTC reads in RC mode')
        _, changed = progress.parse_hardware(reworded)
        self.assertIn('Physical-result anchor no longer matches its update: rtc and watchdog', changed['warnings'])
        sample = '''Every row below has **no qualification evidence**.
- DMA: **implemented**. Host checks remain pending. No host evidence is available.
- CPU: **tested** in a host execution model. Physical work remains untested.
| Hardware | Required functions | Dependencies |
| --- | --- | --- |
| DMA and device address translation | Transfers | Memory |
| CPU cores | Tasks | Clocks |
| UART | Console | Pins |
'''
        parsed, _ = progress.parse_hardware(sample)
        self.assertEqual(parsed[0]['evidence'], [])
        self.assertNotIn('board', parsed[1]['evidence'])
        self.assertIn('host', parsed[1]['evidence'])
        self.assertEqual(parsed[2]['implementation'], 'planned')

    def test_dependency_coverage_and_scoped_edges(self):
        data = json.loads(self.view.snapshot()[0])
        graph = data['dependencies']
        self.assertEqual(graph['status'], 'ok', graph['warnings'])
        nodes = {node['id']: node for node in graph['nodes']}
        inventory = {row['id']: row for row in data['hardware']}
        self.assertEqual({key for key, node in nodes.items() if node['hardware_id']}, set(inventory))
        self.assertEqual(set(nodes['all-board-support']['requires']), set(inventory))
        for key, node in nodes.items():
            self.assertTrue((ROOT / node['record']).is_file())
            for parent in node['requires']:
                self.assertLess(nodes[parent]['level'], node['level'])
            if key in inventory:
                self.assertEqual(node['evidence'], inventory[key]['evidence'])
                self.assertEqual(node['qualification'], inventory[key]['qualification'])
        def ancestors(key):
            return set(nodes[key]['requires']).union(*(ancestors(parent) for parent in nodes[key]['requires']))
        for key in ('gpu', 'npu', 'usb-controllers-and-phys'):
            self.assertNotIn('cpu-cores', ancestors(key))
            self.assertNotIn('system-iommu', ancestors(key))
            self.assertNotIn('dma-controllers', ancestors(key))
        self.assertNotIn('wi-fi', ancestors('bluetooth-controller-and-profiles'))
        self.assertIn('radio-firmware', ancestors('bluetooth-controller-and-profiles'))
        self.assertIn('dma-controllers', ancestors('audio-controllers'))
        self.assertIn('system-iommu', ancestors('display-engine'))
        self.assertEqual(nodes['single-core-runtime']['status'], 'structural')

    def test_dependency_invalid_records_and_cache(self):
        path = self.root / 'docs/hardware-dependencies.md'
        original = path.read_text()
        body, tag = self.view.snapshot()
        invalid = [
            (original.replace('| gpu | GPU |', '| omitted-gpu | GPU |'), 'missing'),
            (original + '\n| extra | Extra | unknown | Scope | Condition | hardware.md |\n', 'Unknown'),
            (original + '\n| loop | Loop | loop | Scope | Condition | hardware.md |\n', 'cycle'),
            (original + '\n| gpu | Duplicate | - | Scope | Condition | hardware.md |\n', 'duplicate'),
            ('', 'unavailable'),
        ]
        for source, warning in invalid:
            path.write_text(source)
            updated, new_tag = self.view.snapshot()
            self.assertNotEqual(tag, new_tag)
            graph = json.loads(updated)['dependencies']
            self.assertEqual(graph['status'], 'invalid')
            self.assertEqual(graph['nodes'], [])
            self.assertIn(warning, ' '.join(graph['warnings']))
        path.write_text(original)
        restored, restored_tag = self.view.snapshot()
        self.assertEqual(json.loads(restored)['dependencies']['status'], 'ok')
        self.assertEqual(self.view.snapshot(), (restored, restored_tag))

    def test_cache_and_changed_format(self):
        one, tag = self.view.snapshot()
        self.assertEqual(self.view.snapshot(), (one, tag))
        self.assertEqual(self.view.rebuilds, 1)
        data = json.loads(one)
        self.assertGreater(len(data['handoff']['next_actions']), 0)
        sample = progress.parse_handoff('## Continuation and evidence reuse\n\nProse.\n\n'
                                        '1. First\n   continued.\n2. Second.\n\nTrailing paragraph.\n')
        self.assertEqual([item['text'] for item in sample['next_actions']], ['First\n   continued.', 'Second.'])
        self.assertEqual(data['summary']['qualified'], 0)
        path = self.root / 'docs/hardware.md'
        path.write_text('# Replaced inventory format\n')
        two, other_tag = self.view.snapshot()
        self.assertNotEqual(tag, other_tag)
        changed = json.loads(two)
        self.assertEqual(changed['parser']['status'], 'unsupported')
        self.assertIsNone(changed['summary']['qualified'])
        self.assertEqual(changed['hardware'], [])
        self.assertNotIn('No subsystem', changed['summary']['detail'])

    def create_queue(self, path=None, halted='test-1', port=None):
        with sqlite3.connect(path or self.queue) as db:
            db.executescript('CREATE TABLE settings(key TEXT,value TEXT); '
                             'CREATE TABLE tests(id TEXT,submission TEXT,state TEXT,owner TEXT,created TEXT);')
            db.execute('INSERT INTO settings VALUES(?,?)', ('coordinator', 'DO-NOT-EXPOSE-SECRET'))
            db.execute('INSERT INTO settings VALUES(?,?)', ('halted', halted))
            if port:
                db.execute('INSERT INTO settings VALUES(?,?)', ('port', port))
            db.execute('INSERT INTO tests VALUES(?,?,?,?,?)',
                       ('test-1', json.dumps({'component': 'Watchdog expiry',
                                             'artifacts': [{'path': '/private/artifact'}]}),
                        'pending', None, '2026-10-16T00:00:00+00:00'))

    def test_queue_has_no_writes_or_secrets(self):
        self.assertFalse(progress.queue_summary(self.queue)['available'])
        self.assertFalse(self.queue.exists())
        self.create_queue()
        before = (hashlib.sha256(self.queue.read_bytes()).hexdigest(), self.queue.stat().st_mtime_ns,
                  sorted(p.name for p in self.root.iterdir()))
        result = progress.queue_summary(self.queue)
        self.assertTrue(result['available'])
        self.assertTrue(result['halted'])
        self.assertEqual(result['counts'], {'pending': 1})
        self.assertNotIn('SECRET', json.dumps(result))
        self.assertNotIn('/private', json.dumps(result))
        self.assertEqual(before, (hashlib.sha256(self.queue.read_bytes()).hexdigest(),
                                 self.queue.stat().st_mtime_ns, sorted(p.name for p in self.root.iterdir())))
        wal = Path(str(self.queue) + '-wal')
        wal.write_bytes(b'pending')
        self.assertFalse(progress.queue_summary(self.queue)['available'])
        wal.unlink()
        original = progress.file_signature
        calls = 0
        def race(path):
            nonlocal calls
            if str(path).endswith('-journal'):
                calls += 1
                if calls > 1:
                    return (1, 100, 1, 1)
            return original(path)
        progress.file_signature = race
        try:
            self.assertFalse(progress.queue_summary(self.queue)['available'])
        finally:
            progress.file_signature = original

    def test_each_board_queue_is_visible(self):
        second = self.root / 'queue-3-3.sqlite3'
        self.create_queue(halted='', port='3-1')
        self.create_queue(second, halted='test-3-3', port='3-1')
        view = progress.Progress(self.root, {'3-1': self.queue, '3-3': second})
        data = json.loads(view.snapshot()[0])
        self.assertEqual([(q['port'], q['halted']) for q in data['queues']], [('3-1', False), ('3-3', True)])
        self.assertTrue(data['queue']['halted'])
        self.assertIn('Port 3-3 halted by test-3-3', data['queue']['reason'])
        self.assertEqual(sorted(item['title'] for item in data['queue']['items']),
                         ['3-1: Watchdog expiry', '3-3: Watchdog expiry'])
        self.assertEqual(data['queue']['counts'], {'pending': 2})
        self.assertNotIn('SECRET', json.dumps(data))
        notes = {q['port']: q['note'] for q in data['queues']}
        self.assertNotIn('bound to', notes['3-1'])
        self.assertIn('bound to USB port 3-1, not 3-3', notes['3-3'])
        unbound = self.root / 'unbound.sqlite3'
        self.create_queue(unbound)
        self.assertIn('records no USB port', progress.queue_summary(unbound, '3-1')['note'])
        missing = json.loads(progress.Progress(self.root, {'3-3': self.root / 'absent'}).snapshot()[0])
        self.assertFalse(missing['queue']['available'])
        self.assertFalse((self.root / 'absent').exists())

    def test_queue_arguments(self):
        self.assertEqual(progress.parse_queues(['/q/a=b.sqlite3', '3-3=/q/c=d.sqlite3']),
                         {'3-1': Path('/q/a=b.sqlite3'), '3-3': Path('/q/c=d.sqlite3')})
        self.assertEqual(progress.parse_queues(['x=/q/e.sqlite3']), {'3-1': Path('x=/q/e.sqlite3')})
        for values in (['/q/a', '3-1=/q/b'], ['3-3=']):
            with self.assertRaises(ValueError):
                progress.parse_queues(values)

    def test_git_worktree_metadata(self):
        git = self.root / 'metadata'
        git.mkdir()
        local = git / 'worktrees/example'
        local.mkdir(parents=True)
        (git / 'refs/heads').mkdir(parents=True)
        (local / 'logs').mkdir()
        sha = 'a' * 40
        (self.root / '.git').write_text('gitdir: metadata/worktrees/example\n')
        (local / 'commondir').write_text('../..\n')
        (local / 'HEAD').write_text('ref: refs/heads/work\n')
        (git / 'refs/heads/work').write_text(sha + '\n')
        (local / 'logs/HEAD').write_text('0' * 40 + ' ' + sha +
                                       ' Name <private@example.test> 1760000000 +0000\tcheckout: moving to work\n')
        value = json.loads(self.view.snapshot()[0])['git']
        self.assertEqual(value['head'], sha)
        self.assertEqual(value['branch'], 'work')
        self.assertEqual(value['commits'][0]['kind'], 'reflog')
        self.assertNotIn('private@', json.dumps(value))
        (git / 'refs/heads/work').write_text('b' * 40 + '\n')
        self.assertEqual(json.loads(self.view.snapshot()[0])['git']['head'], 'b' * 40)

    def test_http_allowlist_origin_symlinks_and_etag(self):
        ui = self.root / 'tools/progress-ui'
        ui.mkdir(parents=True)
        (ui / 'index.html').write_text('<!doctype html><title>Progress</title>')
        (ui / 'app.css').write_text('body { color: black; }')
        outside = self.root / 'secret'
        outside.write_text('SECRET')
        (self.root / 'docs/link.md').symlink_to(outside)
        (ui / 'app.js').symlink_to(outside)
        server = progress.ThreadingHTTPServer(('127.0.0.1', 0), progress.handler(self.view))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        def request(path, headers=None, method='GET'):
            connection = http.client.HTTPConnection('127.0.0.1', server.server_port, timeout=2)
            connection.request(method, path, headers=headers or {})
            reply = connection.getresponse()
            result = reply.status, reply.read(), dict(reply.getheaders())
            connection.close()
            return result
        try:
            status, body, headers = request('/api/progress')
            self.assertEqual(status, 200)
            self.assertEqual(request('/api/progress', {'If-None-Match': headers['ETag']})[0], 304)
            self.assertEqual(request('/')[0], 200)
            self.assertEqual(request('/app.css')[0], 200)
            self.assertEqual(request('/api/document?path=docs%2Fhardware.md')[0], 200)
            for path in ('/.git/config', '/api/document?path=../secret',
                         '/api/document?path=docs%2F..%2Fsecret', '/api/document?path=docs/link.md',
                         '/api/document?path=docs/hardware.md&path=docs/handoff.md', '/app.js'):
                status, body, _ = request(path)
                self.assertEqual(status, 404, path)
                self.assertNotIn(b'SECRET', body)
            self.assertEqual(request('/api/progress', {'Host': 'evil.test'})[0], 403)
            self.assertEqual(request('/api/progress', {'Origin': 'https://evil.test'})[0], 403)
            self.assertEqual(request('/api/progress', {'Origin': f'http://127.0.0.1:{server.server_port}'})[0], 200)
            self.assertEqual(request('/api/progress', {'Host': f'[::1]:{server.server_port}'})[0], 200)
            self.assertEqual(request('/api/progress', method='POST')[0], 501)
            # A symlinked parent directory is refused as well as leaf symlinks.
            (self.root / 'docs').rename(self.root / 'canonical')
            (self.root / 'docs').symlink_to(self.root / 'canonical', target_is_directory=True)
            self.assertEqual(request('/api/document?path=docs/hardware.md')[0], 404)
            value = json.loads(request('/api/progress')[1])
            self.assertEqual(value['parser']['status'], 'unavailable')
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)


if __name__ == '__main__':
    unittest.main()
