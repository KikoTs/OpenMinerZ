"""Exercise the import transaction and path checks without proprietary content."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[1] / 'Import-GameAssets.ps1'


@unittest.skipUnless(sys.platform == 'win32', 'Windows PowerShell importer')
class ImporterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='openminerz-import-test-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.game = self.root / 'owned fixture'
        self.packs = self.root / 'private packs'
        self.runtime = self.root / 'private runtime'
        for name in ['CastleMinerZ.exe', 'steam_api.dll', 'Content/Terrain/Textures.xnb',
                     'Content/Sounds.xgs', 'Content/Sounds.xsb', 'Content/Sounds.xwb',
                     'Content/SoundsStreaming.xwb']:
            path = self.game / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(('fixture: ' + name).encode())
        self.bridge = self.root / 'bridge.ps1'
        self.bridge.write_text('''param($game, $output)
foreach ($name in @('terrain_diffuse.bmp', 'terrain_normal_spec.bmp', 'terrain_metal_light.bmp',
                    'character.omrc', 'font_SmallFont.omfnt', 'screen_BlockUIBack.bmp')) {
    [IO.File]::WriteAllText((Join-Path $output $name), 'generated fixture')
}
if ($env:OPENMINERZ_TEST_LOCK) {
    $global:importTestLock = [IO.File]::Open($env:OPENMINERZ_TEST_LOCK, 'Open', 'ReadWrite', 'None')
}
exit 0
''', encoding='utf-8')

    def run_import(self, *args, lock=None):
        env = os.environ.copy()
        env.pop('OPENMINERZ_TEST_LOCK', None)
        if lock:
            env['OPENMINERZ_TEST_LOCK'] = str(lock)
        return subprocess.run(
            ['powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', str(SCRIPT),
             '-GameDirectory', str(self.game), '-ContentPackRoot', str(self.packs),
             '-RuntimeDirectory', str(self.runtime), '-Bridge', str(self.bridge), *map(str, args)],
            capture_output=True, text=True, timeout=30, env=env)

    def old_pack(self):
        target = self.packs / 'classic'
        target.mkdir(parents=True)
        (target / 'keep.txt').write_text('previous pack', encoding='utf-8')
        self.runtime.mkdir()
        (self.runtime / 'steam_api.dll').write_bytes(b'previous runtime')
        return target

    def assert_previous_survived(self):
        self.assertEqual('previous pack', (self.packs / 'classic/keep.txt').read_text())
        self.assertEqual(b'previous runtime', (self.runtime / 'steam_api.dll').read_bytes())
        self.assertFalse(list(self.packs.glob('.import-*')))
        self.assertFalse(list(self.packs.glob('.previous-*')))
        self.assertFalse(list(self.runtime.glob('.steam-*')))

    def test_validation_is_read_only_and_detects_generation(self):
        result = self.run_import('-ValidateOnly')
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn('Classic', result.stdout)
        self.assertFalse(self.packs.exists())
        result = self.run_import('-Style', 'Modern', '-ValidateOnly')
        self.assertNotEqual(0, result.returncode)
        (self.game / 'Content/Sounds.xwb').unlink()
        self.assertNotEqual(0, self.run_import('-ValidateOnly').returncode)

    def test_unsafe_and_overlapping_destinations_are_refused(self):
        checkout = self.root / 'checkout'
        (checkout / '.git').mkdir(parents=True)
        for destination in [self.game / 'imports', checkout / 'assets', self.runtime / 'nested']:
            self.packs = destination
            with self.subTest(destination=destination):
                result = self.run_import('-ValidateOnly')
                self.assertNotEqual(0, result.returncode, result.stdout)
                self.assertFalse(destination.exists())
        self.packs = checkout / 'out/private/packs'
        self.assertEqual(0, self.run_import('-ValidateOnly').returncode)

    def test_non_directory_target_is_refused(self):
        self.packs.mkdir()
        (self.packs / 'classic').write_text('keep me')
        self.assertNotEqual(0, self.run_import().returncode)
        self.assertEqual('keep me', (self.packs / 'classic').read_text())

    def test_success_retains_previous_pack_and_records_hashes(self):
        self.old_pack()
        before = {p: p.read_bytes() for p in self.game.rglob('*') if p.is_file()}
        result = self.run_import()
        self.assertEqual(0, result.returncode, result.stderr)
        target = self.packs / 'classic'
        self.assertTrue((target / '.private-import').exists())
        manifest = json.loads((target / 'import-manifest.json').read_text())
        for entry in manifest['files']:
            self.assertEqual(entry['sha256'], hashlib.sha256((target / entry['path']).read_bytes()).hexdigest())
        self.assertEqual(4, len(list((target / 'Content').iterdir())))
        previous = list(self.packs.glob('.previous-*'))
        self.assertEqual(1, len(previous))
        self.assertEqual('previous pack', (previous[0] / 'keep.txt').read_text())
        self.assertEqual((self.game / 'steam_api.dll').read_bytes(), (self.runtime / 'steam_api.dll').read_bytes())
        self.assertEqual(before, {p: p.read_bytes() for p in self.game.rglob('*') if p.is_file()})

    def test_failed_bridge_leaves_existing_install_untouched(self):
        self.old_pack()
        self.bridge.write_text('exit 7\n')
        self.assertNotEqual(0, self.run_import().returncode)
        self.assert_previous_survived()

    def test_incomplete_output_does_not_replace_existing_install(self):
        self.old_pack()
        self.bridge.write_text('exit 0\n')
        self.assertNotEqual(0, self.run_import().returncode)
        self.assert_previous_survived()

    def test_locked_runtime_rolls_back_already_replaced_pack(self):
        self.old_pack()
        self.assertNotEqual(0, self.run_import(lock=self.runtime / 'steam_api.dll').returncode)
        self.assert_previous_survived()


if __name__ == '__main__':
    unittest.main()
