"""Export bounded, pointer-free movement settings from the owned game assets."""
import math
import struct
from engine_assets import AssetError

# The ordering is part of DNP1, independent of the original object layout.
MOVEMENT_FIELDS = (
    'RunInertiaTime', 'RunDragFactorNoInput', 'RunDragFactorBoost',
    'RunInertiaSlopeFactor', 'RunInertiaSlopeFactorNoInput',
    'RunInertiaSlopeFactorBoost', 'BoostRange', 'BoostFactor',
    'Gravity', 'AirFrictionFactor', 'JumpGravityFactor', 'JumpAnimTimeFactor',
)
ANIMATION_FIELDS = (
    'AnimSpeedChangePortion', 'MinAnimSpeed', 'BoostMaxAnimSpeed',
    'MovementEaseInTime', 'MovementEaseOutTime',
)


def movement_data(scenes):
    def named(path, name):
        rows = [r.values for r in scenes[path] if r.name == name]
        if len(rows) != 1:
            raise AssetError('Movement data requires one named template: ' + name)
        return rows[0]
    game = named('data/templates/gameconfig.wbr', 'default')
    animation = named('data/templates/animation.wbr', 'default')
    settings = {}
    for fields, values in ((MOVEMENT_FIELDS, game), (ANIMATION_FIELDS, animation)):
        for name in fields:
            value = values[name]
            if len(value) != 1 or type(value[0]) not in (int, float) or not math.isfinite(value[0]):
                raise AssetError('Invalid movement setting: ' + name)
            settings[name] = float(value[0])
    if (settings['RunInertiaTime'] <= 0 or settings['Gravity'] >= 0
            or settings['AirFrictionFactor'] < 0
            or not 0 < settings['AnimSpeedChangePortion'] < 1
            or not 0 < settings['MinAnimSpeed'] <= 1
            or settings['BoostMaxAnimSpeed'] < 1
            or settings['BoostRange'] <= 0
            or any(settings[k] < 0 for k in ('RunDragFactorNoInput', 'RunDragFactorBoost',
                                            'MovementEaseInTime', 'MovementEaseOutTime'))):
        raise AssetError('Movement settings outside supported ranges')
    # Scene arrays are serialized in reverse semantic order.
    movement = list(reversed(animation['MovementAnim']))
    if len(movement) != 2 or any(not x for x in movement):
        raise AssetError('Expected slow and fast authored movement clips')
    clips = {'idle': animation['Idle'][0], 'run': movement[0], 'run_fast': movement[1],
             'boost': animation['Sprint'][0], 'jump': animation['Jump'][0],
             'fall': animation['FallIdle'][0], 'land': animation['EasyLanding'][0],
             'land_hard': animation['HardLanding'][0]}
    if any(not v or '/' in v or '\\' in v or '..' in v for v in clips.values()):
        raise AssetError('Invalid authored movement clip name')
    payload = b'DNP1' + struct.pack('<II', 1, len(settings))
    payload += struct.pack('<' + 'f' * len(settings), *settings.values())
    return settings, clips, payload
