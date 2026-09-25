"""Hardware-free generic dispatcher exercise with a second, tiny executable backend."""
import hashlib
import json
import os
import signal
import time
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
from frameyap.model_files import load_backends


FAKE = '''#!/usr/bin/env python3
import os, struct, sys
read = sys.stdin.buffer.read
write = sys.stdout.buffer.write
assert len(sys.argv) in (3, 4), 'generic backend received unsupported flags'
if os.environ.get('FRAMEYAP_ADVANCED_DEBUG') != '0':
    raise AssertionError('generic launcher must not receive Redux debug opt-in')
if os.environ.get('FAKE_PID_PATH'):
    import signal
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    open(os.environ['FAKE_PID_PATH'], 'w').write(str(os.getpid()))
write(struct.pack('<I', 1) + b'Y'); sys.stdout.buffer.flush()
while True:
    header = read(4)
    if not header: break
    if os.environ.get('FAKE_PID_PATH'):
        import time
        time.sleep(60)
    size, = struct.unpack('<I', header)
    payload = read(size)
    assert payload[:1] == b'T' and len(payload) == 9
    assert os.path.isfile(sys.argv[2] + '/clip.raw')
    response = b'R' + payload[1:] + b'fixture transcript'
    write(struct.pack('<I', len(response)) + response); sys.stdout.buffer.flush()
'''


def frame(payload):
    return struct.pack('<I', len(payload)) + payload


def read_frame(stream):
    size, = struct.unpack('<I', stream.read(4))
    return stream.read(size)


class BackendDispatchTest(unittest.TestCase):
    def test_second_executable_without_cpp_or_model_dependencies(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'assets/backends').mkdir(parents=True)
            (root / 'python/frameyap').mkdir(parents=True)
            (root / 'bin').mkdir()
            (root / 'model/fake').mkdir(parents=True)
            clip = root / 'clip'
            clip.mkdir(mode=0o700)
            (clip / 'clip.raw').write_bytes(b'\0' * (3200 * 4))
            model = root / 'model/fake'
            (model / 'weights.dat').write_bytes(b'fixture')
            fake = root / 'bin/fake'
            fake.write_text(FAKE)
            fake.chmod(0o700)
            manifest = {
                'schema': 1, 'id': 'fake', 'display_name': 'Fake backend',
                'launcher': {'type': 'executable', 'path': 'bin/fake',
                             'arguments': ['{model_dir}', '{clip_dir}', '{threads}'],
                             'protocol': 'frameyap-worker-v1'},
                'model': {'source': 'https://example.org/fake', 'revision': 'test',
                          'files': [{'path': 'weights.dat', 'size': 7,
                                     'sha256': hashlib.sha256(b'fixture').hexdigest()}]},
                'attribution': 'Fixture only', 'license': {'id': 'MIT', 'text': 'Test-only fixture'},
                'requirements': {'cpu': 'None', 'gpu': 'None'}}
            (root / 'assets/backends/fake.json').write_text(json.dumps(manifest))
            self.assertEqual(set(load_backends(root / 'assets/backends')), {'fake'})
            argv = [sys.executable, str(ROOT / 'python/frameyap/backend_worker.py'),
                    '--backend', 'fake', '--root', str(root), '--python', sys.executable,
                    '--manifest-dir', str(root / 'assets/backends'), '--model', str(model),
                    '--clip-dir', str(clip), '--threads', '2']
            env = {**os.environ, 'PYTHONDONTWRITEBYTECODE': '1'}
            with subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=env) as child:
                self.assertEqual(read_frame(child.stdout), b'Y')
                child.stdin.write(frame(b'T' + (7).to_bytes(8, 'little')))
                child.stdin.flush()
                self.assertEqual(read_frame(child.stdout), b'R' + (7).to_bytes(8, 'little') + b'fixture transcript')
                child.stdin.close()
                self.assertEqual(child.wait(timeout=5), 0, child.stderr.read())
            # Debug stays enabled in dispatcher, but arbitrary backend argv/env
            # receives no Redux-specific flag or transcript logging opt-in.
            with subprocess.Popen(argv + ['--advanced-debug'], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env) as child:
                self.assertEqual(read_frame(child.stdout), b'Y')
                child.stdin.close()
                self.assertEqual(child.wait(timeout=5), 0, child.stderr.read())
            (model / 'weights.dat').write_bytes(b'corrupt')
            with subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, env=env) as child:
                self.assertEqual(read_frame(child.stdout), b'FM')
                child.stdin.close()
                self.assertEqual(child.wait(timeout=5), 1)

    def test_status_consent_digest_rejected_before_installer_on_same_id_change(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifests = root / 'backends'
            manifests.mkdir()
            store = root / 'models'
            store.mkdir()
            manifest = {
                'schema': 1, 'id': 'fake', 'display_name': 'Fake',
                'launcher': {'type': 'executable', 'path': 'bin/fake',
                             'arguments': ['{model_dir}', '{clip_dir}'], 'protocol': 'frameyap-worker-v1'},
                'model': {'source': 'https://example.org/original', 'revision': 'v1',
                          'files': [{'path': 'fake', 'size': 1, 'sha256': '0' * 64}]},
                'attribution': 'Test', 'license': {'id': 'MIT', 'text': 'Fixture'},
                'requirements': {'cpu': 'None', 'gpu': 'None'}}
            source = manifests / 'fake.json'
            source.write_text(json.dumps(manifest))
            old = hashlib.sha256(source.read_bytes()).hexdigest()
            service = ROOT / 'scripts/backend-service.py'
            base = [sys.executable, str(service), '--manifest-dir', str(manifests),
                    '--model-store', str(store)]
            status = subprocess.run(base + ['--status'], capture_output=True, text=True, check=True)
            self.assertIn(old, status.stdout)
            self.assertIn('DONE', status.stdout)
            marker = root / 'installer-ran'
            installer = root / 'installer.sh'
            installer.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > ' + str(marker) + '\n')
            install = base + ['--install', '--backend', 'fake', '--installer', str(installer),
                              '--expected-manifest-sha256', old]
            manifest['model']['source'] = 'https://example.org/changed'
            source.write_text(json.dumps(manifest))
            wrong = subprocess.run(install, capture_output=True, text=True)
            self.assertNotEqual(wrong.returncode, 0)
            self.assertFalse(marker.exists(), wrong.stdout)
            self.assertNotIn('DONE', wrong.stdout)
            # Reusing a backend ID cannot silently re-authorize new sources.
            new = hashlib.sha256(source.read_bytes()).hexdigest()
            self.assertNotEqual(new, old)
            self.assertEqual(subprocess.run(install[:-1] + [new], capture_output=True).returncode, 0)
            self.assertTrue(marker.exists())
            passed = marker.read_text().splitlines()
            self.assertEqual(passed[passed.index('--expected-manifest-sha256') + 1], new)

    def test_dispatcher_reaps_child_ignoring_term_on_eof_and_native_style_stop(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'assets/backends').mkdir(parents=True)
            (root / 'bin').mkdir()
            model = root / 'model'
            model.mkdir()
            (model / 'weights.dat').write_bytes(b'fixture')
            clip = root / 'clip'
            clip.mkdir(mode=0o700)
            (clip / 'clip.raw').write_bytes(b'\0' * 12800)
            fake = root / 'bin/fake'
            fake.write_text(FAKE)
            fake.chmod(0o700)
            manifest = {'schema': 1, 'id': 'fake', 'display_name': 'Fake',
                        'launcher': {'type': 'executable', 'path': 'bin/fake',
                                     'arguments': ['{model_dir}', '{clip_dir}'], 'protocol': 'frameyap-worker-v1'},
                        'model': {'source': 'https://example.org/fake', 'revision': 'test',
                                  'files': [{'path': 'weights.dat', 'size': 7,
                                             'sha256': hashlib.sha256(b'fixture').hexdigest()}]},
                        'attribution': 'Fixture', 'license': {'id': 'MIT', 'text': 'Fixture'},
                        'requirements': {'cpu': 'None', 'gpu': 'None'}}
            (root / 'assets/backends/fake.json').write_text(json.dumps(manifest))
            args = [sys.executable, str(ROOT / 'python/frameyap/backend_worker.py'),
                    '--backend', 'fake', '--root', str(root), '--python', sys.executable,
                    '--manifest-dir', str(root / 'assets/backends'), '--model', str(model),
                    '--clip-dir', str(clip), '--threads', '2']
            for mode in ('eof', 'term'):
                pidfile = root / ('pid-' + mode)
                with subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, start_new_session=True,
                                      env={**os.environ, 'FAKE_PID_PATH': str(pidfile)}) as child:
                    self.assertEqual(read_frame(child.stdout), b'Y')
                    pid = int(pidfile.read_text())
                    if mode == 'eof':
                        child.stdin.close()
                    else:
                        child.stdin.write(frame(b'T' + (1).to_bytes(8, 'little')))
                        child.stdin.flush()
                        child.terminate()
                    try:
                        code = child.wait(timeout=3)
                        self.assertIn(code, (0, 1), child.stderr.read())
                        for _ in range(100):
                            if not Path('/proc/' + str(pid)).exists():
                                break
                            time.sleep(0.01)
                        self.assertFalse(Path('/proc/' + str(pid)).exists(), 'owned model still running')
                    finally:
                        # Same dedicated group as native. Kill only this test's processes.
                        try:
                            os.killpg(child.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass


if __name__ == '__main__':
    unittest.main()
