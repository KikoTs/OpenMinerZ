"""Release-boundary regression tests; no owned game installation is required."""
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('release', Path(__file__).parents[1] / 'release.py')
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


def glb(document):
    text = json.dumps(document).encode()
    text += b' ' * (-len(text) % 4)
    return struct.pack('<4sIIII', b'glTF', 2, len(text) + 20, len(text), 0x4E4F534A) + text


class ReleaseBoundaryTests(unittest.TestCase):
    def test_private_paths_are_rejected_at_any_depth(self):
        for name in ['Content/Sounds.xwb', 'a/DerivedData/terrain.png', 'out/frame.bmp',
                     '.workspace/settings.json', 'nested/.workspace/notes.txt', 'a/.private-import', '.env.local',
                     'foo/steam_api.dll', '../outside.cpp', 'C:/outside.cpp', 'a\\b.cpp']:
            with self.subTest(name=name):
                self.assertIsNotNone(release.forbidden_path(name))
        self.assertIsNone(release.forbidden_path('.github/workflows/source-audit.yml'))

    def test_runtime_allowlist_does_not_allow_proprietary_dlls(self):
        self.assertIsNone(release.forbidden_path('SDL3.dll', runtime=True))
        self.assertIsNone(release.forbidden_path('ImportTools/OpenMinerZ.ContentBridge.exe', runtime=True))
        self.assertIsNotNone(release.forbidden_path('steam_api.dll', runtime=True))
        self.assertIsNotNone(release.forbidden_path('CastleMinerZ.exe', runtime=True))
        self.assertIsNotNone(release.forbidden_path('other/SDL3.dll', runtime=True))

    def test_reference_motion_and_skin_are_not_authored_animation(self):
        name = 'examples/mods/pack/assets/animations/reload.glb'
        self.assertEqual([], release.gltf_issues(name, glb({'nodes': [{'name': 'Root'}], 'animations': [{'name': 'Reload'}]})))
        for document in [{'animations': [{'name': 'REF_Reload'}]}, {'skins': [{}]},
                         {'meshes': [{}]}, {'nodes': [{'translation': [0, 0, 0]}]}]:
            with self.subTest(document=document):
                self.assertTrue(release.gltf_issues(name, glb(document)))

    def test_malformed_model_is_rejected(self):
        self.assertTrue(release.gltf_issues('x.glb', b'not a glb'))

    def test_media_requires_review_and_an_exact_hash(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); path = root / 'example.wav'; path.write_bytes(b'original recording')
            self.assertTrue(release.audit_files(root, ['example.wav'], {}))
            approved = {'example.wav': {'origin': 'my generator', 'license': 'AGPL-3.0-or-later',
                                        'sha256': release.sha256(path.read_bytes())}}
            self.assertEqual([], release.audit_files(root, ['example.wav'], approved))
            path.write_bytes(b'replaced recording')
            self.assertTrue(release.audit_files(root, ['example.wav'], approved))

    def test_disguised_import_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); (root / 'picture.dat').write_bytes(b'XNBw' + bytes(20))
            self.assertTrue(release.audit_files(root, ['picture.dat'], {}))

    def test_removed_approved_media_cannot_silently_disappear(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertTrue(release.audit_files(Path(folder), [], {'missing.wav': {}}))

    def test_document_links_and_fingerprint(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); doc = root / 'README.md'
            doc.write_text('[guide](guide.md)\n', encoding='utf-8')
            self.assertTrue(release.audit_links(root, ['README.md']))
            (root / 'guide.md').write_text('Guide\n', encoding='utf-8')
            self.assertEqual([], release.audit_links(root, ['README.md', 'guide.md']))
            before = release.fingerprint(root, ['README.md', 'guide.md'])
            (root / 'guide.md').write_text('Changed\n', encoding='utf-8')
            self.assertNotEqual(before, release.fingerprint(root, ['README.md', 'guide.md']))


if __name__ == '__main__':
    unittest.main()
