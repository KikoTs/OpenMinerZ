"""Rig Lab transient previews, optimistic saves and transactional exports."""
from __future__ import annotations

import copy
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

import rig_lab as lab

RIG = lab.WORK / 'rig.glb'


@unittest.skipUnless(RIG.exists(), 'Requires the locally exported Rig Lab skeleton')
class RigLabBackendTests(unittest.TestCase):
    def setUp(self):
        self.previous = copy.deepcopy(lab.authoring.ACTIVE or lab.profile_state()[0])
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        work = self.root / 'work'
        work.mkdir()
        shutil.copy2(RIG, work / 'rig.glb')
        self.profile = lab.authoring.defaults()
        self.path = self.root / 'profile.json'
        self.path.write_text(json.dumps(self.profile))
        self.patches = [patch.object(lab.authoring, 'PROFILE_PATH', self.path),
                        patch.multiple(lab, WORK=work, HISTORY=work / 'history',
                                       EXPORT_STATUS=work / 'export-status.json',
                                       PACK=self.root / 'source', PLAYABLE=self.root / 'runtime')]
        for patched in self.patches:
            patched.start()
        for folder in (lab.PACK, lab.PLAYABLE):
            folder.mkdir()
            (folder / 'old.txt').write_text(folder.name)
        lab._rig_bones.cache_clear()
        lab._clip_data_cached.cache_clear()
        lab.authoring.configure(lab.animation, self.profile)

    def tearDown(self):
        for patched in reversed(self.patches):
            patched.stop()
        lab.authoring.configure(lab.animation, self.previous)
        lab._rig_bones.cache_clear()
        lab._clip_data_cached.cache_clear()
        self.temp.cleanup()

    def test_pose_preview_is_transient_fast_path_and_restores_globals(self):
        draft = copy.deepcopy(self.profile)
        draft['palms']['Left'] = [.08, -.03, 0]
        draft['finger_curl']['Index'] = [12, 23, 34]
        original = self.path.read_bytes()
        palm = dict(lab.animation.PALM)
        curl = dict(lab.animation._CURL)
        with patch.object(lab.animation, 'evaluate', side_effect=AssertionError('Must not bake a clip')), \
             patch.object(lab, 'weapon', side_effect=AssertionError('Must not export textures')), \
             patch.object(Path, 'write_text', side_effect=AssertionError('Preview must not write')):
            result = lab.preview({'name': 'LMGReload', 'profile': draft, 'time': 4., 'requestId': 29})
        self.assertEqual(result['previewMode'], 'pose')
        self.assertEqual(result['requestId'], 29)
        self.assertEqual(len(result['frame']['rotations']), 71)
        self.assertIn('box_new', result['frame']['parts'])
        self.assertEqual(len(result['frame']['targetRotation']), 4)
        self.assertEqual(len(result['frame']['wristRotation']), 4)
        self.assertEqual(self.path.read_bytes(), original)
        self.assertFalse(lab.HISTORY.exists())
        self.assertEqual(lab.authoring.ACTIVE, self.profile)
        self.assertEqual(lab.animation.PALM, palm)
        self.assertEqual(lab.animation._CURL, curl)
        with self.assertRaises(ValueError):
            lab.preview({'name': 'does-not-exist', 'profile': draft, 'time': 0})
        self.assertEqual(lab.animation.PALM, palm)
        self.assertEqual(lab.animation._CURL, curl)

    def test_full_preview_and_single_pose_expose_linked_support(self):
        draft = copy.deepcopy(self.profile)
        draft['models'] = {'modern/ak': {'support': {'position': [0, .005, 0]}}}
        single = lab.preview({'name': 'GunIdle', 'profile': draft, 'time': 0})
        full = lab.preview({'name': 'GunIdle', 'profile': draft})
        self.assertEqual(full['previewMode'], 'clip')
        self.assertEqual(len(full['frames']), lab.animation.frame_count(full['length']))
        self.assertEqual(single['frame']['rotations'], full['frames'][0]['rotations'])
        self.assertTrue(single['supportsLinked'])
        self.assertTrue(single['supportKeys'])
        self.assertIn('left', single['gripKeys'])
        self.assertIn('thumb_opposition', single['gripKeys']['left'][0])

    def test_interactive_carried_parts_start_and_seat_at_actual_socket(self):
        with lab.draft_profile(self.profile):
            skeleton, _, clip = lab.find_clip('GunReload')
            for name, (_, start, end, _) in clip.part_follow.items():
                contact = end if name.endswith('_new') else start
                _, _, parts = lab.solve_single(skeleton, clip, contact)
                self.assertLess(lab.animation.v_len(parts[name].p), .0001)
                self.assertLess(lab.animation.q_angle(parts[name].q), .001)

    def test_retimed_key_origins_survive_single_and_full_preview_metadata(self):
        baseline = lab.preview({'name': 'GunReload', 'profile': self.profile})
        # Mirror the HTTP round-trip: tuple-valued authored transforms become
        # JSON arrays before the browser submits an edited profile.
        left = json.loads(json.dumps(baseline['keys'][1]))
        gun = json.loads(json.dumps(baseline['gunKeys'][1]))
        for key in (left, gun):
            key['sourceTime'] = key['time']
            key['time'] += .013
        draft = copy.deepcopy(self.profile)
        draft['clips'] = {'GunReload': {'left': [left], 'gun': [gun]}}
        for extra in ({'time': left['time']}, {}):
            result = lab.preview({'name': 'GunReload', 'profile': draft, **extra})
            for key, field in ((left, 'keys'), (gun, 'gunKeys')):
                moved = next(item for item in result[field] if abs(item['time'] - key['time']) < 1e-8)
                self.assertEqual(moved['sourceTime'], key['sourceTime'])
                self.assertFalse(any(abs(item['time'] - key['sourceTime']) < 1e-8 for item in result[field]))
        restored = lab.preview({'name': 'GunReload', 'profile': self.profile, 'time': 0})
        self.assertEqual(restored['keys'], baseline['keys'])
        self.assertEqual(restored['gunKeys'], baseline['gunKeys'])

    def test_parallel_drafts_are_serialized_and_do_not_leak(self):
        def request(offset):
            draft = copy.deepcopy(self.profile)
            draft['models'] = {'modern/ak': {'support': {'position': [0, offset, 0]}}}
            return lab.preview({'name': 'GunIdle', 'profile': draft, 'time': 0})['frame']['target']
        expected = [request(value) for value in (0, .01, -.01)]
        with ThreadPoolExecutor(max_workers=3) as pool:
            actual = list(pool.map(request, (0, .01, -.01)))
        self.assertEqual(actual, expected)
        self.assertNotEqual(actual[0], actual[1])
        self.assertEqual(lab.authoring.ACTIVE, self.profile)

    def test_save_snapshots_previous_profile_and_rejects_stale_revision(self):
        old, revision = lab.profile_state()
        changed = copy.deepcopy(old)
        changed['palms']['Left'][0] += .001
        saved = lab.save_profile(changed, revision)
        self.assertNotEqual(saved['revision'], revision)
        self.assertEqual(lab.history_entry(saved['history'][0]['id'])['profile'], old)
        self.assertEqual(lab.profile_state()[0], changed)
        with self.assertRaises(lab.ProfileConflict):
            lab.save_profile(old, revision)
        self.assertEqual(len(lab.history_list()), 1)
        self.assertEqual(lab.profile_state()[0], changed)

    def test_invalid_save_does_not_create_history_or_change_disk(self):
        _, revision = lab.profile_state()
        invalid = copy.deepcopy(self.profile)
        invalid['clips'] = {'unknown-clip': {'left': []}}
        with self.assertRaises(ValueError):
            lab.save_profile(invalid, revision)
        self.assertEqual(lab.profile_state()[1], revision)
        self.assertFalse(lab.HISTORY.exists())

    def test_history_ids_cannot_escape_and_initial_backup_is_readable(self):
        lab.HISTORY.mkdir()
        initial = 'before-editor-upgrade-20261016-035840'
        (lab.HISTORY / (initial + '.json')).write_text(json.dumps(self.profile))
        self.assertEqual(lab.history_entry(initial)['profile'], self.profile)
        self.assertEqual(lab.history_list()[0]['id'], initial)
        for identifier in ('../profile', '..\\profile', '/profile', initial + '.json', 'x'):
            with self.assertRaises(ValueError):
                lab.history_entry(identifier)

    def test_failed_generation_or_validation_preserves_both_packs(self):
        original = [lab.tree_hashes(folder) for folder in (lab.PACK, lab.PLAYABLE)]
        for codes in ((1,), (0, 1)):
            with patch.object(lab.subprocess, 'run', side_effect=[subprocess.CompletedProcess([], code, 'out', 'err') for code in codes]):
                result = lab.build_pack({})
            self.assertFalse(result['ok'])
            self.assertEqual([lab.tree_hashes(folder) for folder in (lab.PACK, lab.PLAYABLE)], original)
            self.assertFalse(lab.EXPORT_STATUS.exists())
            self.assertEqual(list((lab.WORK / 'builds').iterdir()), [])

    def test_successful_build_publishes_equal_copies_and_actual_status(self):
        def command(args, **kwargs):
            if 'make_tactical_animations.py' in args[1]:
                (Path(args[-1]) / 'generated.txt').write_text('new generated data')
            return subprocess.CompletedProcess(args, 0, 'ok\n', '')
        with patch.object(lab.subprocess, 'run', side_effect=command):
            result = lab.build_pack({'profile': self.profile, 'revision': lab.profile_state()[1]})
        self.assertTrue(result['ok'])
        self.assertEqual(lab.tree_hashes(lab.PACK), lab.tree_hashes(lab.PLAYABLE))
        self.assertTrue(result['export']['sourceMatchesPlayable'])
        self.assertFalse(result['export']['outOfDate'])
        self.assertEqual(result['export']['files'], 2)
        (lab.PLAYABLE / 'generated.txt').write_text('externally changed')
        self.assertFalse(lab.export_status()['sourceMatchesPlayable'])
        self.assertTrue(lab.export_status()['outOfDate'])

    def test_external_profile_change_during_build_does_not_publish(self):
        originals = [lab.tree_hashes(folder) for folder in (lab.PACK, lab.PLAYABLE)]
        def command(args, **kwargs):
            changed = copy.deepcopy(self.profile)
            changed['palms']['Left'][0] += .001
            self.path.write_text(json.dumps(changed))
            return subprocess.CompletedProcess(args, 0, '', '')
        with patch.object(lab.subprocess, 'run', side_effect=command):
            result = lab.build_pack({})
        self.assertFalse(result['ok'])
        self.assertIn('changed during export', result['log'])
        self.assertEqual([lab.tree_hashes(folder) for folder in (lab.PACK, lab.PLAYABLE)], originals)

    def test_publish_rolls_back_first_target_if_second_swap_fails(self):
        staged = self.root / 'stage'
        staged.mkdir()
        (staged / 'new.txt').write_text('new')
        originals = [lab.tree_hashes(folder) for folder in (lab.PACK, lab.PLAYABLE)]
        original_rename = Path.rename
        def rename(path, target):
            if '.riglab-new-' in path.name and Path(target) == lab.PLAYABLE:
                raise OSError('Simulated locked runtime directory')
            return original_rename(path, target)
        with patch.object(Path, 'rename', rename), self.assertRaises(OSError):
            lab.publish_pack(staged, (lab.PACK, lab.PLAYABLE))
        self.assertEqual([lab.tree_hashes(folder) for folder in (lab.PACK, lab.PLAYABLE)], originals)
        self.assertEqual(list(self.root.glob('*.riglab-*')), [])

    def test_publish_rolls_back_if_recording_export_fails(self):
        staged = self.root / 'stage'
        staged.mkdir()
        (staged / 'new.txt').write_text('new')
        originals = [lab.tree_hashes(folder) for folder in (lab.PACK, lab.PLAYABLE)]
        def fail(_):
            raise OSError('Simulated status write failure')
        with self.assertRaises(OSError):
            lab.publish_pack(staged, (lab.PACK, lab.PLAYABLE), fail)
        self.assertEqual([lab.tree_hashes(folder) for folder in (lab.PACK, lab.PLAYABLE)], originals)

    def test_http_preview_revision_conflict_history_and_server_identity(self):
        server = lab.ThreadingHTTPServer(('127.0.0.1', 0), lab.Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = f'http://127.0.0.1:{server.server_port}'
        def post(path, document, origin=url):
            request = Request(url + path, data=json.dumps(document).encode(),
                              headers={'Content-Type': 'application/json', 'Origin': origin})
            with urlopen(request, timeout=5) as response:
                return json.load(response)
        try:
            self.assertTrue(lab.existing_server(server.server_port))
            with urlopen(url + '/api/identity') as response:
                self.assertEqual(json.load(response)['sourceRevision'], lab.SOURCE_REVISION)
            with patch.object(lab.sys, 'argv', ['rig_lab.py', '--port', str(server.server_port), '--open']), \
                 patch.object(lab, 'ThreadingHTTPServer', side_effect=AssertionError('Must reuse server')), \
                 patch.object(lab.webbrowser, 'open') as open_browser:
                lab.main()
                open_browser.assert_called_once_with(url)
            with urlopen(url + '/api/catalog') as response:
                catalog = json.load(response)
            self.assertEqual(catalog['defaults'], lab.authoring.defaults())
            result = post('/api/preview', {'name': 'GunIdle', 'profile': self.profile, 'time': 0, 'requestId': 'drag-2'})
            self.assertEqual(result['requestId'], 'drag-2')
            saved = post('/api/profile', {'profile': self.profile, 'revision': catalog['revision']})
            with self.assertRaises(HTTPError) as raised:
                post('/api/profile', {'profile': self.profile, 'revision': 'stale'})
            self.assertEqual(raised.exception.code, 409)
            self.assertEqual(json.load(raised.exception)['revision'], saved['revision'])
            identifier = saved['history'][0]['id']
            with urlopen(url + '/api/history?id=' + identifier) as response:
                self.assertEqual(json.load(response)['profile'], self.profile)
            with self.assertRaises(HTTPError) as raised:
                post('/api/preview', {'name': 'GunIdle', 'profile': self.profile, 'time': 0}, origin='http://foreign.invalid')
            self.assertEqual(raised.exception.code, 403)
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == '__main__':
    unittest.main()
