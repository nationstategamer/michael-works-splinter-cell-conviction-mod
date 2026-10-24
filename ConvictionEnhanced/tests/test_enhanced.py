"""Isolated safety and merge tests. Never invokes or writes to the live game."""
from __future__ import annotations

import importlib.util
import itertools
import json
from pathlib import Path
import tempfile
import sys
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

import synthetic_fixtures as synthetic

PACKAGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PACKAGE))
import local_baselines
SPEC = importlib.util.spec_from_file_location("conviction_enhanced_under_test", PACKAGE / "enhanced.py")
enhanced = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(enhanced)

FUSION_INI = (
    b"; user configuration\r\n[GAMEPLAY]\r\n"
    b"DisableNegativeMouseAcceleration = 0 ; preserve this comment\r\n"
    b"DisableMouseSmoothing=0\r\nRawMouseInput=0\r\n"
    b"BlacklistControlScheme=0\r\nUnrelatedUserChoice=73\r\n"
    b"[OTHER]\r\nRawMouseInput=9\r\n"
)
LEGACY_BYTES = b"fixture-only legacy ASI bytes\x00\xff"
UPDATE = "src/system/update/"
AIM_SEQUENCE_XML = synthetic.AIM_SEQUENCE_XML
PRESS_TRACK_BYTES = synthetic.PRESS_TRACK_BYTES


class IsolatedGame(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="conviction-enhanced-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = (Path(self.temp.name) / "game").resolve()
        self.root.mkdir()
        fixture_package = Path(self.temp.name) / "package"
        (fixture_package / "baselines").mkdir(parents=True)
        (fixture_package / "baselines/hashes.json").write_text(json.dumps(synthetic.hashes()), "utf-8")
        baseline_source = Path(self.temp.name) / "owned-local-dump"
        baseline_source.mkdir()
        for name, data in synthetic.BASELINES.items():
            (baseline_source / name).write_bytes(data)
        local_baselines.import_baselines(self.root, baseline_source,
                                        catalog_path=fixture_package / "baselines/hashes.json")
        catalog = json.loads((PACKAGE / "compatibility.json").read_text("utf-8"))
        for relative in catalog["blacklist_assets"]:
            data = (AIM_SEQUENCE_XML if relative == enhanced.AIM_SEQUENCE else
                    ("fixture asset: " + relative + "\r\n").encode("ascii"))
            self.write(relative, data)
            catalog["blacklist_assets"][relative] = enhanced.digest(data)
        (fixture_package / "compatibility.json").write_text(json.dumps(catalog), "utf-8")
        self.package_patch = mock.patch.object(enhanced, "PACKAGE", fixture_package)
        self.package_patch.start()
        self.addCleanup(self.package_patch.stop)
        # Identity checks are deliberately bypassed for fake executable fixtures.
        # Actual baseline hashing, INI merge, transaction and recovery stay real.
        self.identity = mock.patch.object(enhanced.Manager, "inspect", autospec=True)
        self.identity.start()
        self.addCleanup(self.identity.stop)
        self.running = mock.patch.object(enhanced, "game_running", return_value=False)
        self.running.start()
        self.addCleanup(self.running.stop)
        self.write(enhanced.FUSION, FUSION_INI)
        self.write(enhanced.LEGACY, LEGACY_BYTES)
        self.write(UPDATE + "actionscheme.ini", b"[Control]\r\nFixture=unchanged\r\n")
        self.write(UPDATE + "weapontuning.ini", b"[Weapon]\r\nm_bUnlimitedAmmo=false\r\n")
        self.write(UPDATE + "convictionsettings.ini", b"[Network]\r\nSyncMaxStepFrequency=60\r\n")
        self.manager = enhanced.Manager(self.root)

    def write(self, relative, value):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value)

    def read(self, relative):
        path = self.root / relative
        return path.read_bytes() if path.exists() else None

    def baseline(self, filename):
        return synthetic.BASELINES[filename]

    def snapshot(self):
        return {
            path.relative_to(self.root).as_posix(): path.read_bytes()
            for path in self.root.rglob("*")
            if path.is_file() and "ConvictionEnhanced-state" not in path.parts
        }

    def test_all_four_module_combinations_install_and_restore(self):
        original = self.snapshot()
        for tactical, deniable in itertools.product((False, True), repeat=2):
            modules = set()
            if tactical:
                modules.add("tactical")
            if deniable:
                modules.add("deniable")
            with self.subTest(modules=modules):
                report = self.manager.install(modules)
                self.assertEqual(report["modules"], ["comfort"] + sorted(modules))
                self.assertFalse(report["runtime_validated"])
                self.assertIsNotNone(self.read(UPDATE + "convictioncamera.ini"))
                self.assertEqual(self.read(UPDATE + "aiconfig.ini") is not None, tactical)
                self.assertEqual(self.read(UPDATE + "healthtuning.ini") is not None, tactical)
                self.assertEqual(self.read(UPDATE + "huntermatchmanager.ini") is not None, deniable)
                self.assertEqual(self.read(UPDATE + "laststandmatchmanager.ini") is not None, deniable)
                self.assertIsNone(self.read(enhanced.LEGACY))
                self.manager.uninstall()
                self.assertEqual(self.snapshot(), original)
                self.assertIsNone(self.manager.active())

    def test_plan_is_read_only_and_unrelated_fusion_options_survive(self):
        original = self.snapshot()
        local_before = {path.name: path.read_bytes() for path in (self.manager.store / 'baselines').iterdir()}
        output, report = self.manager.plan({"tactical", "deniable"})
        self.assertEqual(self.snapshot(), original)
        self.assertFalse((self.manager.store / "active.json").exists())
        self.assertEqual({path.name: path.read_bytes() for path in (self.manager.store / 'baselines').iterdir()}, local_before)
        self.assertIn(b"UnrelatedUserChoice=73\r\n", output[enhanced.FUSION])
        self.assertIn(b"[OTHER]\r\nRawMouseInput=9\r\n", output[enhanced.FUSION])
        self.assertIn(b"DisableNegativeMouseAcceleration = 1 ; preserve this comment\r\n", output[enhanced.FUSION])
        self.assertTrue(all(name not in output for name in (
            UPDATE + "actionscheme.ini", UPDATE + "weapontuning.ini", UPDATE + "convictionsettings.ini"
        )))

    def test_standard_mouse_preserved_across_module_switches_and_exact_uninstall(self):
        original = self.snapshot()
        self.manager.install({'tactical', 'deniable'}, mouse_input='standard')
        cfg = self.read(enhanced.FUSION)
        self.assertIn(b'\r\nRawMouseInput=0\r\n', cfg)
        self.assertIn(b'DisableMouseSmoothing=1', cfg)
        self.assertIn(b'DisableNegativeMouseAcceleration = 1', cfg)
        self.manager.install(set())
        self.assertEqual(self.manager.active()['selection']['mouse_input'], 'standard')
        self.assertIn(b'\r\nRawMouseInput=0\r\n', self.read(enhanced.FUSION))
        self.manager.install({'tactical'}, mouse_input='raw')
        self.assertIn(b'\r\nRawMouseInput=1\r\n', self.read(enhanced.FUSION))
        self.manager.uninstall()
        self.assertEqual(self.snapshot(), original)

    def test_standard_mouse_changes_only_one_flag_from_raw_profile(self):
        raw, _ = self.manager.plan({'tactical', 'deniable'}, mouse_input='raw')
        standard, _ = self.manager.plan({'tactical', 'deniable'}, mouse_input='standard')
        self.assertEqual(set(raw), set(standard))
        for name in raw:
            if name == enhanced.FUSION:
                self.assertEqual(standard[name], raw[name].replace(b'\r\nRawMouseInput=1\r\n', b'\r\nRawMouseInput=0\r\n', 1))
            else:
                self.assertEqual(raw[name], standard[name])

    def test_tactical_preserves_every_other_ai_byte_and_array_order(self):
        output, _ = self.manager.plan({"tactical"})
        original = self.baseline("aiconfig.ini")
        expected = original.replace(b"m_fTimeToGoBackToPatrol=40.0;", b"m_fTimeToGoBackToPatrol=60.0;")
        self.assertEqual(original.count(b"fSearchActiveDuration=40,"), 3)
        expected = expected.replace(b"fSearchActiveDuration=40,", b"fSearchActiveDuration=60,")
        actual = output[UPDATE + "aiconfig.ini"]
        self.assertEqual(actual, expected)
        rows = [line for line in actual.splitlines() if line.startswith(b"m_GlobalAIParametersByClass=")]
        self.assertEqual(len(rows), 5)
        self.assertTrue(all(b"fSearchActiveDuration=60," in line for line in rows))

    def test_health_changes_are_only_out_of_combat_rate_and_delay(self):
        output, _ = self.manager.plan({"tactical"})
        before = self.baseline("healthtuning.ini")
        expected = before.replace(b"\nRegenRate=80", b"\nRegenRate=40", 1)
        expected = expected.replace(b"\nRegenDelay=0", b"\nRegenDelay=3", 1)
        self.assertEqual(output[UPDATE + "healthtuning.ini"], expected)
        self.assertIn(b"CombatRegenRate=80", expected)
        self.assertIn(b"CombatRegenDelay=10", expected)
        self.assertIn(b"RegenRateMultiplayer=240", expected)
        self.assertIn(b"UseAcceleratingRegen=true", expected)

    def test_deniable_keeps_all_twenty_waves_and_spawn_limits_exact(self):
        output, _ = self.manager.plan({"deniable"})
        original = self.baseline("laststandmatchmanager.ini")
        actual = output[UPDATE + "laststandmatchmanager.ini"]
        expected = original.replace(b"m_fTimeBetweenWaves=25.0f;", b"m_fTimeBetweenWaves=15.0f;")
        self.assertEqual(actual, expected)
        rows_before = [line for line in original.splitlines(keepends=True) if line.startswith(b"m_arrLastStandWaveInfo=")]
        rows_after = [line for line in actual.splitlines(keepends=True) if line.startswith(b"m_arrLastStandWaveInfo=")]
        self.assertEqual(len(rows_after), 20)
        self.assertEqual(rows_before, rows_after)
        hunter = self.baseline("huntermatchmanager.ini")
        self.assertEqual(output[UPDATE + "huntermatchmanager.ini"],
                         hunter.replace(b"m_iDetectionReinforcementSizeSolo=2;", b"m_iDetectionReinforcementSizeSolo=3;"))

    def test_switching_modules_off_removes_originally_absent_overrides(self):
        self.manager.install({"tactical", "deniable"})
        self.manager.install(set())
        for name in ("aiconfig.ini", "healthtuning.ini", "huntermatchmanager.ini", "laststandmatchmanager.ini"):
            self.assertIsNone(self.read(UPDATE + name), name)
        self.assertIsNotNone(self.read(UPDATE + "convictioncamera.ini"))
        # Selecting again must use the real original baseline, not a removed file.
        self.manager.install({"tactical"})
        self.assertIsNotNone(self.read(UPDATE + "aiconfig.ini"))
        self.assertIsNone(self.read(UPDATE + "huntermatchmanager.ini"))

    def test_switching_modules_off_restores_existing_complete_overrides(self):
        existing = {}
        for name in ("aiconfig.ini", "healthtuning.ini", "huntermatchmanager.ini", "laststandmatchmanager.ini"):
            existing[name] = self.baseline(name) + b"\r\n; existing custom trailing comment \xff\r\n"
            self.write(UPDATE + name, existing[name])
        self.manager.install({"tactical", "deniable"})
        self.manager.install(set())
        for name, original in existing.items():
            self.assertEqual(self.read(UPDATE + name), original, name)

    def test_preexisting_untargeted_tuning_is_preserved(self):
        source = self.baseline("healthtuning.ini")
        custom = source.replace(b"InitialHealth=1000", b"InitialHealth=1200")
        custom += b"\r\n[PrivateCustomSection]\r\nKeep=unchanged\xff\r\n"
        self.write(UPDATE + "healthtuning.ini", custom)
        self.manager.install({"tactical"})
        actual = self.read(UPDATE + "healthtuning.ini")
        expected = custom.replace(b"\nRegenRate=80", b"\nRegenRate=40", 1).replace(b"\nRegenDelay=0", b"\nRegenDelay=3", 1)
        self.assertEqual(actual, expected)
        self.manager.uninstall()
        self.assertEqual(self.read(UPDATE + "healthtuning.ini"), custom)

    def test_targeted_changed_missing_and_duplicate_values_refuse_without_writes(self):
        before = self.baseline("healthtuning.ini")
        lines = before.splitlines(keepends=True)
        targeted = next(line for line in lines if line.startswith(b"RegenRate=80"))
        cases = {
            "changed": before.replace(targeted, targeted.replace(b"RegenRate=80", b"RegenRate=72")),
            "missing": before.replace(targeted, b""),
            "duplicate": before.replace(targeted, targeted + targeted),
        }
        for label, data in cases.items():
            with self.subTest(label=label):
                self.write(UPDATE + "healthtuning.ini", data)
                original = self.snapshot()
                with self.assertRaises(enhanced.Conflict):
                    self.manager.install({"tactical"})
                self.assertEqual(self.snapshot(), original)
                self.assertIsNone(self.manager.active())
                self.assertFalse((self.manager.store / "pending.json").exists())

    def test_reordered_array_count_and_nested_target_conflicts_are_rejected(self):
        original = self.baseline("aiconfig.ini")
        rows = [line for line in original.splitlines(keepends=True) if line.startswith(b"m_GlobalAIParametersByClass=")]
        for label, data in {
            "missing row": original.replace(rows[1], b"", 1),
            "extra row": original.replace(rows[1], rows[1] + rows[1], 1),
            "changed nested value": original.replace(b"fSearchActiveDuration=40,", b"fSearchActiveDuration=45,", 1),
        }.items():
            with self.subTest(label=label):
                self.write(UPDATE + "aiconfig.ini", data)
                with self.assertRaises(enhanced.Conflict):
                    self.manager.plan({"tactical"})

    def test_uninstall_restores_legacy_and_every_original_byte(self):
        for name in enhanced.FILES:
            self.write(UPDATE + name, self.baseline(name) + b"\r\n; preserved user footer\r\n")
        original = self.snapshot()
        self.manager.install({"tactical", "deniable"})
        self.manager.install({"deniable"}, fov=100, deadzone=0.2, controls="original")
        self.manager.uninstall()
        self.assertEqual(self.snapshot(), original)
        self.assertEqual(self.read(enhanced.LEGACY), LEGACY_BYTES)

    def test_user_edits_after_install_block_switch_and_uninstall(self):
        self.manager.install({"tactical"})
        relative = UPDATE + "convictioncamera.ini"
        self.write(relative, self.read(relative) + b"\r\n; edit after installation\r\n")
        snapshot = self.snapshot()
        for operation in (lambda: self.manager.install(set()), self.manager.uninstall):
            with self.assertRaisesRegex(enhanced.Conflict, "Changed since installation"):
                operation()
            self.assertEqual(self.snapshot(), snapshot)

    def test_missing_blacklist_file_rejects_blacklist_but_allows_original_controls(self):
        (self.root / (UPDATE + "blacklist.ini")).unlink()
        with self.assertRaisesRegex(enhanced.Conflict, "Missing or changed Fusion Blacklist control asset"):
            self.manager.plan(set())
        output, _ = self.manager.plan(set(), controls="original")
        self.assertIn(b"BlacklistControlScheme=0", output[enhanced.FUSION])

    def test_write_failure_rolls_back_all_targets_and_state(self):
        original = self.snapshot()
        real_write = self.manager.write_target
        calls = 0

        def fail_once(relative, data):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("simulated disk error")
            real_write(relative, data)

        with mock.patch.object(self.manager, "write_target", side_effect=fail_once):
            with self.assertRaisesRegex(OSError, "simulated disk error"):
                self.manager.install({"tactical", "deniable"})
        self.assertEqual(self.snapshot(), original)
        self.assertIsNone(self.manager.active())
        self.assertFalse((self.manager.store / "pending.json").exists())
        self.assertFalse((self.manager.store / "operation.lock").exists())

    def interrupt_install(self):
        real_write = self.manager.write_target
        calls = 0

        def interrupt_second_write(relative, data):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise KeyboardInterrupt("simulated process interruption")
            real_write(relative, data)

        with mock.patch.object(self.manager, "write_target", side_effect=interrupt_second_write):
            with self.assertRaises(KeyboardInterrupt):
                self.manager.install({"tactical", "deniable"})
        self.assertTrue((self.manager.store / "pending.json").exists())

    def test_interrupted_transaction_recover_restores_originals(self):
        original = self.snapshot()
        self.interrupt_install()
        self.assertNotEqual(self.snapshot(), original)
        with self.assertRaisesRegex(enhanced.Conflict, "Interrupted transaction"):
            self.manager.plan(set())
        with self.manager.locked():
            self.manager.recover_unlocked()
        self.assertEqual(self.snapshot(), original)
        self.assertIsNone(self.manager.active())
        self.assertFalse((self.manager.store / "pending.json").exists())

    def test_recovery_never_overwrites_subsequent_user_edits(self):
        self.interrupt_install()
        relative = UPDATE + "convictioncamera.ini"
        self.write(relative, self.read(relative) + b"\r\n; user recovery-time edit\r\n")
        snapshot = self.snapshot()
        with self.manager.locked():
            with self.assertRaisesRegex(enhanced.Conflict, "subsequent edit"):
                self.manager.recover_unlocked()
        self.assertEqual(self.snapshot(), snapshot)
        self.assertTrue((self.manager.store / "pending.json").exists())

    def test_corrupted_original_backup_blocks_uninstall_without_changes(self):
        self.manager.install({"tactical"})
        sha = self.manager.active()["files"][enhanced.FUSION]["original"]
        (self.manager.store / "blobs" / sha).write_bytes(b"corrupted backup")
        snapshot = self.snapshot()
        with self.assertRaisesRegex(enhanced.Conflict, "Backup integrity"):
            self.manager.uninstall()
        self.assertEqual(self.snapshot(), snapshot)
        self.assertIsNotNone(self.manager.active())

    def test_corrupted_recovery_backup_preflights_before_any_restoration(self):
        self.interrupt_install()
        pending = json.loads((self.manager.store / "pending.json").read_text("utf-8"))
        sha = pending["files"][enhanced.FUSION]["before"]
        (self.manager.store / "blobs" / sha).write_bytes(b"corrupted recovery data")
        snapshot = self.snapshot()
        with self.manager.locked():
            with self.assertRaisesRegex(enhanced.Conflict, "Backup integrity"):
                self.manager.recover_unlocked()
        self.assertEqual(self.snapshot(), snapshot)
        self.assertTrue((self.manager.store / "pending.json").exists())

    def test_running_game_blocks_install_and_preserves_every_target(self):
        original = self.snapshot()
        with mock.patch.object(enhanced, "game_running", return_value=True):
            with self.assertRaisesRegex(enhanced.Conflict, "Close Conviction"):
                self.manager.install(set())
        self.assertEqual(self.snapshot(), original)
        self.assertIsNone(self.manager.active())


    def test_reordered_class_array_is_rejected(self):
        source = self.baseline("aiconfig.ini")
        rows = [line for line in source.splitlines(keepends=True) if line.startswith(b"m_GlobalAIParametersByClass=")]
        reordered = source.replace(rows[0], b"UNIQUE_TEST_ROW_PLACEHOLDER", 1)
        reordered = reordered.replace(rows[1], rows[0], 1)
        reordered = reordered.replace(b"UNIQUE_TEST_ROW_PLACEHOLDER", rows[1], 1)
        self.write(UPDATE + "aiconfig.ini", reordered)
        with self.assertRaisesRegex(enhanced.Conflict, "m_GlobalAIParametersByClass"):
            self.manager.plan({"tactical"})

    def test_restored_optional_file_is_no_longer_owned_by_manager(self):
        relative = UPDATE + "aiconfig.ini"
        self.write(relative, self.baseline("aiconfig.ini"))
        self.manager.install({"tactical"})
        self.manager.install(set())
        self.assertNotIn(relative, self.manager.active()["files"])
        edited = self.read(relative) + b"\r\n; modified after module was removed\r\n"
        self.write(relative, edited)
        self.manager.uninstall()
        self.assertEqual(self.read(relative), edited)

    def test_final_blacklist_asset_is_verified_as_well_as_main_ini(self):
        catalog = json.loads((enhanced.PACKAGE / "compatibility.json").read_text("utf-8"))
        relative = list(catalog["blacklist_assets"])[-1]
        self.assertNotEqual(relative, UPDATE + "blacklist.ini")
        self.write(relative, b"modified asset")
        with self.assertRaisesRegex(enhanced.Conflict, "Missing or changed Fusion Blacklist"):
            self.manager.plan(set())

    def test_aim_release_plan_adds_only_one_release_track_and_is_read_only(self):
        original = self.snapshot()
        output, report = self.manager.plan({'tactical', 'deniable'}, mouse_input='standard')
        self.assertEqual(self.snapshot(), original)
        self.assertFalse((self.manager.store / "active.json").exists())
        repaired = output[enhanced.AIM_SEQUENCE]
        document = ET.fromstring(repaired)
        timeline = document.find("Task[@name='Start Narrative Walk']/Timeline")
        tracks = list(timeline)
        release = [track for track in tracks if track.get('name') == 'MW_ReleaseAim']
        self.assertEqual(len(release), 1)
        release = release[0]
        previous = tracks[tracks.index(release) - 1]
        self.assertEqual(previous.get('name'), 'NewTrack_21')
        command = release.find('Event/CommandInstance')
        self.assertEqual(command.get('name'), 'PlayerEnableButton')
        self.assertEqual(command.find("ParameterInstance[@name='ActionType']").get('data'),
                         'Repository::Unreal::enums::eActionType::eActionType_ReleaseLeftTrigger')
        self.assertEqual(command.find("ParameterInstance[@name='Enable']/Boolean").get('value'), '1')
        release_bytes = ET.tostring(release, encoding='utf-8')
        # Remove the one inserted track by its raw boundaries, not by XML
        # serialization: every original line/comment/restriction must survive.
        start = repaired.index(b'      <Track name="MW_ReleaseAim"')
        end = repaired.index(b'      </Track>\r\n', start) + len(b'      </Track>\r\n')
        self.assertEqual(repaired[:start] + repaired[end:], AIM_SEQUENCE_XML)
        self.assertIn(b'eActionType_ReleaseLeftTrigger', release_bytes)
        catalog = json.loads((enhanced.PACKAGE / 'compatibility.json').read_text())
        for relative in catalog['blacklist_assets']:
            if relative != enhanced.AIM_SEQUENCE:
                self.assertNotIn(relative, output)
                self.assertEqual(self.read(relative), original[relative])
        self.assertTrue(any(change.get('file') == enhanced.AIM_SEQUENCE for change in report['changes']))

    def test_aim_repair_is_owned_repeatable_and_exactly_reversible(self):
        original = self.snapshot()
        self.manager.install({'tactical'}, mouse_input='standard')
        repaired = self.read(enhanced.AIM_SEQUENCE)
        record = self.manager.active()['files'][enhanced.AIM_SEQUENCE]
        self.assertEqual(self.manager.blob(record['original']), AIM_SEQUENCE_XML)
        self.assertEqual(record['installed'], enhanced.digest(repaired))
        self.manager.check_controls()
        self.manager.install({'tactical', 'deniable'}, mouse_input='standard')
        self.assertEqual(self.read(enhanced.AIM_SEQUENCE), repaired)
        self.assertEqual(repaired.count(b'eActionType_ReleaseLeftTrigger'), 1)
        self.manager.check_controls()
        self.manager.uninstall()
        self.assertEqual(self.snapshot(), original)

    def test_original_controls_restore_aim_asset_and_release_ownership(self):
        self.manager.install(set())
        self.assertNotEqual(self.read(enhanced.AIM_SEQUENCE), AIM_SEQUENCE_XML)
        self.manager.install(set(), controls='original')
        self.assertEqual(self.read(enhanced.AIM_SEQUENCE), AIM_SEQUENCE_XML)
        self.assertNotIn(enhanced.AIM_SEQUENCE, self.manager.active()['files'])
        changed = AIM_SEQUENCE_XML + b'<!-- user edit after restoring original controls -->\r\n'
        self.write(enhanced.AIM_SEQUENCE, changed)
        self.manager.uninstall()
        self.assertEqual(self.read(enhanced.AIM_SEQUENCE), changed)

    def test_blacklist_original_blacklist_roundtrip_accepts_pinned_baseline(self):
        self.manager.install(set())
        repaired = self.read(enhanced.AIM_SEQUENCE)
        self.manager.install(set(), controls='original')
        self.assertEqual(self.read(enhanced.AIM_SEQUENCE), AIM_SEQUENCE_XML)
        self.manager.install(set(), controls='blacklist')
        self.assertEqual(self.read(enhanced.AIM_SEQUENCE), repaired)
        self.manager.check_controls()
        # Also accept a conservative older record retaining ownership of an
        # already restored, byte-identical pinned baseline.
        self.manager.install(set(), controls='original')
        active = self.manager.active()
        sha = self.manager.save_blob(AIM_SEQUENCE_XML)
        active['files'][enhanced.AIM_SEQUENCE] = {'original': sha, 'installed': sha}
        self.manager.write_active(active)
        self.manager.check_controls()
        self.manager.install(set(), controls='blacklist')
        self.assertEqual(self.read(enhanced.AIM_SEQUENCE), repaired)
        self.manager.check_controls()
        self.manager.uninstall()
        self.assertEqual(self.read(enhanced.AIM_SEQUENCE), AIM_SEQUENCE_XML)

    def test_unowned_already_repaired_asset_is_not_adopted(self):
        self.write(enhanced.AIM_SEQUENCE, enhanced.repair_aim_release(AIM_SEQUENCE_XML))
        original = self.snapshot()
        with self.assertRaisesRegex(enhanced.Conflict, 'Missing or changed Fusion Blacklist'):
            self.manager.install(set())
        self.assertEqual(self.snapshot(), original)
        self.assertIsNone(self.manager.active())

    def test_owned_aim_asset_edit_blocks_switch_and_uninstall(self):
        self.manager.install(set())
        self.write(enhanced.AIM_SEQUENCE, self.read(enhanced.AIM_SEQUENCE) + b'<!-- later edit -->')
        original = self.snapshot()
        for operation in (lambda: self.manager.install(set(), controls='original'),
                          self.manager.uninstall, self.manager.check_controls):
            with self.assertRaises(enhanced.Conflict):
                operation()
            self.assertEqual(self.snapshot(), original)

    def test_owned_aim_repair_requires_pinned_original_and_exact_repair(self):
        self.manager.install(set())
        active = self.manager.active()
        altered_original = AIM_SEQUENCE_XML.replace(b'keep the rest', b'edited the rest')
        altered_repair = enhanced.repair_aim_release(AIM_SEQUENCE_XML).replace(b'keep the rest', b'edited the rest')
        self.write(enhanced.AIM_SEQUENCE, altered_repair)
        active['files'][enhanced.AIM_SEQUENCE] = {
            'original': self.manager.save_blob(altered_original),
            'installed': enhanced.digest(altered_repair),
        }
        self.manager.write_active(active)
        self.manager.check_active(active)  # both hashes and blobs are coherent
        with self.assertRaisesRegex(enhanced.Conflict, 'Missing or changed Fusion Blacklist'):
            self.manager.check_controls()  # but the original is not pinned Fusion

    def test_write_failure_after_aim_repair_restores_original_asset(self):
        original = self.snapshot()
        write = self.manager.write_target
        wrote_repair = False
        failed = False
        def fail_after_repair(relative, data):
            nonlocal wrote_repair, failed
            if wrote_repair and not failed:
                failed = True
                raise OSError('fixture failure after aim repair')
            write(relative, data)
            if relative == enhanced.AIM_SEQUENCE:
                wrote_repair = True
        with mock.patch.object(self.manager, 'write_target', side_effect=fail_after_repair):
            with self.assertRaisesRegex(OSError, 'after aim repair'):
                self.manager.install({'tactical'})
        self.assertTrue(wrote_repair)
        self.assertTrue(failed)
        self.assertEqual(self.snapshot(), original)
        self.assertIsNone(self.manager.active())

    def test_interrupted_install_after_aim_repair_recovers_exactly(self):
        original = self.snapshot()
        write = self.manager.write_target
        wrote_repair = False
        def interrupt_after_repair(relative, data):
            nonlocal wrote_repair
            if wrote_repair:
                raise KeyboardInterrupt('fixture interruption after aim repair')
            write(relative, data)
            if relative == enhanced.AIM_SEQUENCE:
                wrote_repair = True
        with mock.patch.object(self.manager, 'write_target', side_effect=interrupt_after_repair):
            with self.assertRaises(KeyboardInterrupt):
                self.manager.install({'tactical'})
        self.assertNotEqual(self.read(enhanced.AIM_SEQUENCE), AIM_SEQUENCE_XML)
        with self.manager.locked():
            self.manager.recover_unlocked()
        self.assertEqual(self.snapshot(), original)
        self.assertIsNone(self.manager.active())

    def test_incomplete_override_refused_before_install(self):
        self.write(UPDATE + "healthtuning.ini", b"[Health_DefaultPlayer]\nRegenRate=80\nRegenDelay=0\n")
        original = self.snapshot()
        with self.assertRaisesRegex(enhanced.Conflict, "incomplete or structurally incompatible"):
            self.manager.install({"tactical"})
        self.assertEqual(self.snapshot(), original)

    def test_local_baseline_cache_hash_is_checked(self):
        (self.manager.store / "baselines" / "convictioncamera.ini").write_bytes(b"bad cached data")
        original = self.snapshot()
        with self.assertRaisesRegex(enhanced.Conflict, "Local baseline cache integrity"):
            self.manager.install(set())
        self.assertEqual(self.snapshot(), original)

    def test_missing_local_cache_blocks_presets_without_touching_existing_overrides(self):
        for name in enhanced.FILES:
            self.write(UPDATE + name, self.baseline(name))
        (self.manager.store / 'baselines/convictioncamera.ini').unlink()
        original = self.snapshot()
        with self.assertRaisesRegex(enhanced.Conflict, 'Import the five pristine'):
            self.manager.install(set(), controls='original')
        self.assertEqual(self.snapshot(), original)
        self.assertIsNone(self.manager.active())

    def test_pending_recovery_is_bound_to_original_game_folder(self):
        self.interrupt_install()
        pending_path = self.manager.store / "pending.json"
        pending = json.loads(pending_path.read_text("utf-8"))
        pending["root"] = str(self.root.parent / "different-game")
        pending_path.write_text(json.dumps(pending), "utf-8")
        original = self.snapshot()
        with self.manager.locked():
            with self.assertRaisesRegex(enhanced.Conflict, "different game folder"):
                self.manager.recover_unlocked()
        self.assertEqual(self.snapshot(), original)
        self.assertTrue(pending_path.exists())

    def test_failed_reconfiguration_restores_active_selection_and_prior_files(self):
        self.manager.install({"tactical"})
        original = self.snapshot()
        active = self.manager.active()
        real_write = self.manager.write_target
        calls = 0

        def fail_once(relative, data):
            nonlocal calls
            calls += 1
            if calls == 3:
                raise OSError("simulated switch failure")
            real_write(relative, data)

        with mock.patch.object(self.manager, "write_target", side_effect=fail_once):
            with self.assertRaisesRegex(OSError, "simulated switch failure"):
                self.manager.install({"deniable"}, fov=100)
        self.assertEqual(self.snapshot(), original)
        self.assertEqual(self.manager.active(), active)
        self.assertFalse((self.manager.store / "pending.json").exists())


class BytePatching(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="conviction-public-byte-test-")
        self.addCleanup(self.temp.cleanup)
        self.fixture_package = Path(self.temp.name)
        synthetic.write_catalog(self.fixture_package, enhanced.AIM_SEQUENCE)
        patcher = mock.patch.object(enhanced, "PACKAGE", self.fixture_package)
        patcher.start()
        self.addCleanup(patcher.stop)

    def trust_input(self, data):
        # Synthetic metadata exercises structural validation after the hash gate.
        synthetic.write_catalog(self.fixture_package, enhanced.AIM_SEQUENCE, data)

    def test_aim_transform_uses_local_byte_span_and_preserves_mixed_formatting(self):
        block = PRESS_TRACK_BYTES.replace(b'"', b"'").replace(b'      ', b'\t').replace(b'\r\n', b'\n')
        block = block.replace(b'\t</Track>', b'\t<!-- synthetic </Track> text -->\n\t</Track>')
        source = AIM_SEQUENCE_XML.replace(PRESS_TRACK_BYTES, block)
        self.trust_input(source)
        actual = enhanced.repair_aim_release(source)
        release = block.replace(b"name='NewTrack_21'", b"name='MW_ReleaseAim'", 1).replace(
            b'eActionType_PressLeftTrigger', b'eActionType_ReleaseLeftTrigger', 1)
        where = source.index(block) + len(block)
        self.assertEqual(actual, source[:where] + release + source[where:])
        self.assertEqual(len(ET.fromstring(actual).findall('.//Track')), 3)

    def test_aim_hash_gate_rejects_other_local_bytes_before_transform(self):
        changed = AIM_SEQUENCE_XML.replace(b'keep the rest', b'other local text')
        with self.assertRaisesRegex(enhanced.Conflict, 'verified pinned original'):
            enhanced.repair_aim_release(changed)

    def test_aim_document_types_and_entities_refuse_even_with_synthetic_pin(self):
        source = AIM_SEQUENCE_XML.replace(b'<Sequence', b'<!DOCTYPE Sequence [<!ENTITY test "value">]><Sequence', 1)
        self.trust_input(source)
        with self.assertRaisesRegex(enhanced.Conflict, 'document types or entities'):
            enhanced.repair_aim_release(source)

    def test_aim_press_schema_remains_strict_after_hash_verification(self):
        variants = (
            PRESS_TRACK_BYTES.replace(b'PlayerEnableButton', b'OtherCommand'),
            PRESS_TRACK_BYTES.replace(b'<Boolean value="1"', b'<Boolean value="0"'),
            PRESS_TRACK_BYTES.replace(b'entity="Repository::o_Character_Sam"', b'entity="Repository::Other"'),
            PRESS_TRACK_BYTES.replace(b'<ParameterInstance name="Character"',
                                     b'<ParameterInstance name="Extra"/><ParameterInstance name="Character"'),
        )
        for block in variants:
            with self.subTest(block=block):
                source = AIM_SEQUENCE_XML.replace(PRESS_TRACK_BYTES, block)
                self.trust_input(source)
                with self.assertRaises(enhanced.Conflict):
                    enhanced.repair_aim_release(source)

    def test_aim_repair_rejects_missing_duplicate_or_wrong_scene_schema(self):
        bad = {
            'wrong scene': AIM_SEQUENCE_XML.replace(b'BB03_NarrativeWalk', b'OtherScene'),
            'wrong task': AIM_SEQUENCE_XML.replace(b'Start Narrative Walk', b'Another Task'),
            'wrong first task': AIM_SEQUENCE_XML.replace(b'first="1"', b'first="0"'),
            'wrong timeline': AIM_SEQUENCE_XML.replace(b'name="NewTimeline"', b'name="OtherTimeline"'),
            'all buttons enabled': AIM_SEQUENCE_XML.replace(b'<Boolean value="0"/>', b'<Boolean value="1"/>'),
            'missing press': AIM_SEQUENCE_XML.replace(PRESS_TRACK_BYTES, b''),
            'duplicate press': AIM_SEQUENCE_XML.replace(PRESS_TRACK_BYTES,
                                                       PRESS_TRACK_BYTES * 2),
            'different enabled character': AIM_SEQUENCE_XML.replace(b'data="Repository::o_Character_Sam"',
                                                                      b'data="Repository::Other"', 1),
            'already repaired': enhanced.repair_aim_release(AIM_SEQUENCE_XML),
            'malformed XML': AIM_SEQUENCE_XML[:-15],
        }
        for name, data in bad.items():
            self.trust_input(data)
            with self.subTest(name=name), self.assertRaises(enhanced.Conflict):
                enhanced.repair_aim_release(data)

    def test_crlf_non_ascii_comments_repeated_sections_and_array_order_survive(self):
        original = (
            b"; opaque comment \xff\r\n[Target]\r\n"
            b"Rows=(keep=7, value=40, name=first) // value=999\r\n"
            b"[Other]\r\nRows=(value=66)\r\n"
            b"[Target]\r\nRows=(keep=8,value=60,name=second)\r\n"
        )
        actual = enhanced.patch(original, "Target", "Rows", "40", "60",
                                field="value", occurrence=0, count=2)
        self.assertEqual(actual, original.replace(b"value=40", b"value=60"))

    def test_ambiguous_nested_fields_are_rejected(self):
        with self.assertRaises(enhanced.Conflict):
            enhanced.patch(b"[A]\nRows=(v=40,v=40)\n", "A", "Rows", "40", "60", field="v")

    def test_already_applied_values_are_idempotent(self):
        original = b"[A]\r\nValue=60; // keep\r\n"
        self.assertEqual(enhanced.patch(original, "A", "Value", "40", "60"), original)


if __name__ == "__main__":
    unittest.main()
