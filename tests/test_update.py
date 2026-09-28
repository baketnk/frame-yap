"""Offline checks for the manual release lookup and attended installer handoff."""
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / 'scripts' / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class UpdateTests(unittest.TestCase):
    def test_tags(self):
        check = load('frameyap_update', 'check-update.py')
        current = '0.1.202609282232'
        self.assertEqual(check.outcome('{"tag_name":"v0.1.202609292232"}', current),
                         'AVAILABLE 0.1.202609292232')
        for tag in ('v0.1.202609282232', 'v0.1.202609272232'):
            self.assertEqual(check.outcome('{"tag_name":"' + tag + '"}', current), 'CURRENT')
        for tag in ('v0.1.202613292232', '0.1.202609292232', 'v0.1.202609292232;sh',
                    'v0.1.20260929', '../v0.1.202609292232'):
            with self.assertRaises(ValueError, msg=tag):
                check.outcome('{"tag_name":"' + tag + '"}', current)
        with self.assertRaises(ValueError):
            check.outcome('{"tag_name":"v0.1.202609292232"}', '0.1.invalid')

    def test_network_only_on_explicit_check(self):
        check = load('frameyap_update_2', 'check-update.py')
        with patch.object(check.request, 'build_opener') as build:
            with self.assertRaises(SystemExit):
                check.main(['--current', '0.1.202609282232'])
            build.assert_not_called()
        class Response:
            def __enter__(self): return self
            def __exit__(self, *args): return False
            def read(self, limit):
                self.limit = limit
                return b'{"tag_name":"v0.1.202609292232"}'
        class Opener:
            def open(self, req, timeout):
                self.url, self.timeout = req.full_url, timeout
                return Response()
        opener = Opener()
        with patch.object(check.request, 'build_opener', return_value=opener):
            self.assertEqual(check.check('0.1.202609282232'), 'AVAILABLE 0.1.202609292232')
        self.assertEqual(opener.url, 'https://api.github.com/repos/baketnk/frame-yap/releases/latest')
        self.assertEqual(opener.timeout, 6)

    def test_terminal_command_is_fixed_and_version_checked(self):
        helper = load('frameyap_installer_handoff', 'install-update.py')
        cmd = helper.installer_command(Path('/owned/bin/install.sh'), '0.1.202609292232')
        self.assertEqual(cmd, ['sh', '/owned/bin/install.sh', '--mode', 'binary',
                               '--version', '0.1.202609292232', '--yes'])
        for bad in ('0.1.202609292232;rm', 'v0.1.202609292232', 'latest'):
            with self.assertRaises(ValueError):
                helper.installer_command(Path('/owned/bin/install.sh'), bad)

    def test_terminal_handoff_requires_tty_before_install(self):
        helper = load('frameyap_installer_handoff_2', 'install-update.py')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'scripts').mkdir()
            (root / 'bin').mkdir()
            (root / 'bin/install.sh').write_text('#!/bin/sh\nexit 0\n')
            helper.__file__ = str(root / 'scripts/install-update.py')
            with patch.object(helper.sys, 'stdin', io.StringIO('\n')), patch.object(helper.subprocess, 'run') as run:
                self.assertEqual(helper.main(['--version', '0.1.202609292232']), 1)
                run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
