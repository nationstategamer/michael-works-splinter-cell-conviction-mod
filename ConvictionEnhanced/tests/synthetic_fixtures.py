"""Hand-authored tiny test inputs, not extracted game files.

Only functional section/key/interface identifiers used by the patcher are
represented. Array values and XML formatting are synthetic fixture choices.
"""
from __future__ import annotations

import hashlib
import json


def ini(*lines):
    return ('; Synthetic public test fixture\r\n' + '\r\n'.join(lines) + '\r\n').encode('ascii')


BASELINES = {
    'convictioncamera.ini': ini('[Default]', 'FOV=(v=90,FixtureKeep=7)',
                                '[Echelon.ECamera]', 'm_inputDeadZone=0.25', 'SyntheticCameraFlag=unchanged'),
    'aiconfig.ini': ini('[Echelon.EAIManager]', 'm_fTimeToGoBackToPatrol=40.0;',
                       *('m_GlobalAIParametersByClass=(iNbMaxFlank=%d,fSearchActiveDuration=%d,FixtureClass=%d)'
                         % (index, 40 if index <= 3 else 60, index * 11) for index in range(1, 6))),
    'healthtuning.ini': ini('[Health_DefaultPlayer]', 'InitialHealth=1000', 'RegenRate=80', 'RegenDelay=0',
                           'CombatRegenRate=80', 'CombatRegenDelay=10', 'RegenRateMultiplayer=240',
                           'UseAcceleratingRegen=true'),
    'huntermatchmanager.ini': ini('[EchelonAI.ECoopHunterSpawner]', 'm_iDetectionReinforcementSizeSolo=2;',
                                 'SyntheticHunterChoice=unchanged'),
    'laststandmatchmanager.ini': ini('[Echelon.ELastStandMatchManager]', 'm_fTimeBetweenWaves=25.0f;',
                                    *('m_arrLastStandWaveInfo=(FixtureWave=%d,FixtureCount=%d)' % (n, n + 7)
                                      for n in range(1, 21))),
}

# This minimal document is assembled for tests, not copied from a game scene.
PRESS_TRACK_BYTES = (
    b'      <Track name="NewTrack_21" entity="Repository::o_Character_Sam">\r\n'
    b'        <Event name="SyntheticPress" end="-1.000000"><CommandInstance name="PlayerEnableButton">\r\n'
    b'          <ParameterInstance name="Character" data="Repository::o_Character_Sam"/>\r\n'
    b'          <ParameterInstance name="ActionType" data="Repository::Unreal::enums::eActionType::eActionType_PressLeftTrigger"/>\r\n'
    b'          <ParameterInstance name="Enable"><Boolean value="1"/></ParameterInstance>\r\n'
    b'        </CommandInstance></Event>\r\n'
    b'      </Track>\r\n'
)
AIM_SEQUENCE_XML = (
    b'<?xml version="1.0" encoding="utf-8" ?>\r\n<Sequence name="BB03_NarrativeWalk">\r\n'
    b'  <Task name="Start Narrative Walk" first="1" fixture="public-synthetic">\r\n'
    b'    <Timeline name="NewTimeline">\r\n'
    b'      <Track name="SyntheticDisable"><Event><CommandInstance name="PlayerEnableAllButtons">\r\n'
    b'        <ParameterInstance name="Character" data="Repository::o_Character_Sam"/>\r\n'
    b'        <ParameterInstance name="Enable"><Boolean value="0"/></ParameterInstance>\r\n'
    b'      </CommandInstance></Event></Track>\r\n'
    + PRESS_TRACK_BYTES
    + b'      <!-- keep the rest of the synthetic scene byte-identical -->\r\n'
      b'    </Timeline>\r\n  </Task>\r\n</Sequence>\r\n'
)


def hashes():
    return {name: hashlib.sha256(data).hexdigest() for name, data in BASELINES.items()}


def write_catalog(folder, aim_path, aim_data=AIM_SEQUENCE_XML):
    """Create only metadata in a disposable package; full INIs stay elsewhere."""
    (folder / 'baselines').mkdir(parents=True, exist_ok=True)
    (folder / 'baselines/hashes.json').write_text(json.dumps(hashes()), encoding='utf-8')
    (folder / 'compatibility.json').write_text(json.dumps({
        'blacklist_assets': {aim_path: hashlib.sha256(aim_data).hexdigest()}}), encoding='utf-8')
