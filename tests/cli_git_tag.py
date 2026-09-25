"""Verify build metadata suppression without touching the repository's tags/history."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GitVersionTest(unittest.TestCase):
    def test_clean_matching_tag_suppresses_git_line(self):
        if not shutil.which('git'):
            self.skipTest('Git unavailable')
        # Isolate only CMake's version block in a tiny, disposable Git tree.
        source = (ROOT / 'CMakeLists.txt').read_text()
        header = source.split('message(STATUS "FrameYap build version:', 1)[0]
        version = '2.3.202601020304'
        with tempfile.TemporaryDirectory(prefix='frameyap-tag-test-') as temp:
            root = Path(temp)
            (root / 'CMakeLists.txt').write_text(header + '\nfile(WRITE "${CMAKE_CURRENT_BINARY_DIR}/git-info.txt" "${FRAMEYAP_GIT_INFO}")\n')
            # CMake's source-tree dirty check includes untracked files. Ignore
            # only the disposable build directory in this mini repository.
            (root / '.gitignore').write_text('build/\n')
            def git(*args):
                subprocess.run(['git', '-C', str(root), *args], check=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            def configure():
                subprocess.run(['cmake', '-S', str(root), '-B', str(root / 'build'),
                                '-DFRAMEYAP_VERSION=' + version], check=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
                return (root / 'build/git-info.txt').read_text()
            git('init', '-q')
            git('add', 'CMakeLists.txt', '.gitignore')
            git('-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
                'commit', '-qm', 'fixture')
            git('tag', 'v' + version)
            self.assertEqual(configure(), '')
            (root / 'dirty.txt').write_text('local change\n')
            self.assertRegex(configure(), r'^git [0-9a-f]+ \(uncommitted changes\)$')
            (root / 'dirty.txt').unlink()
            git('tag', '-d', 'v' + version)
            self.assertRegex(configure(), r'^git [0-9a-f]+$')


if __name__ == '__main__':
    unittest.main()
