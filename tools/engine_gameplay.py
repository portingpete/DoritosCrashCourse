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
PLAYER_FIELDS = ('JumpMaxQueueTime', 'LandRunSpeedFactor', 'LandResumeBufferTime')
CURVE_FIELDS = ('JumpDeviationFactors', 'JumpDeviationSpeeds', 'AirDeviationFactors', 'AirDeviationSpeeds')


def movement_data(scenes):
    def named(path, name):
        rows = [r.values for r in scenes[path] if r.name == name]
        if len(rows) != 1:
            raise AssetError('Movement data requires one named template: ' + name)
        return rows[0]
    game = named('data/templates/gameconfig.wbr', 'default')
    animation = named('data/templates/animation.wbr', 'default')
    settings = {}
    for fields, values in ((MOVEMENT_FIELDS, game), (ANIMATION_FIELDS, animation), (PLAYER_FIELDS, game)):
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
            or settings['JumpGravityFactor'] >= 0 or not 0 <= settings['JumpAnimTimeFactor'] < 1
            or any(settings[k] < 0 for k in ('RunDragFactorNoInput', 'RunDragFactorBoost',
                                            'MovementEaseInTime', 'MovementEaseOutTime', *PLAYER_FIELDS))):
        raise AssetError('Movement settings outside supported ranges')
    # Scene arrays are serialized in reverse semantic order.
    movement = list(reversed(animation['MovementAnim']))
    if len(movement) != 2 or any(not x for x in movement):
        raise AssetError('Expected slow and fast authored movement clips')
    clips = {'idle': animation['Idle'][0], 'run': movement[0], 'run_fast': movement[1],
             'boost': animation['Sprint'][0], 'jump': animation['Jump'][0],
             'fall': animation['FallIdle'][0], 'land': animation['EasyLanding'][0],
             'land_hard': animation['HardLanding'][0],
             'startup_usa': animation['StartUpUsaIdle'][0],
             'startup_europe': animation['StartUpEuropeIdle'][0],
             'startup_japan': animation['StartUpJapanIdle'][0]}
    if any(not v or '/' in v or '\\' in v or '..' in v for v in clips.values()):
        raise AssetError('Invalid authored movement clip name')
    extra = []
    for field in CURVE_FIELDS:
        rows=game[field]
        if len(rows)!=1 or len(rows[0])!=2 or not all(math.isfinite(v) for v in rows[0]):
            raise AssetError('Invalid airborne control curve: '+field)
        if field.endswith('Speeds') and not 0<=rows[0][0]<rows[0][1]:
            raise AssetError('Airborne speed curve must increase')
        extra.extend(rows[0]);settings[field]=rows[0]
    fighter=named('data/templates/fighter.wbr','player/gamepad0')
    primitive=named('data/templates/cprimitive3.wbr',fighter['FeetCollider'][0])
    if primitive['PrimitiveType']!=[1] or fighter['FeetColliderBone']!=['']:
        raise AssetError('Course player requires the authored unattached sphere')
    low,high=primitive['BoundingBoxMin'][0],primitive['BoundingBoxMax'][0]
    scale=fighter['FeetColliderScale'][0];offset=fighter['FeetColliderPos'][0]
    if (any(abs(a+b)>.0001 for a,b in zip(low,high)) or len(low)!=3 or len(high)!=3
            or len(scale)!=3 or len(offset)!=3 or any(v<=0 for v in high+scale)
            or not all(math.isfinite(v) for v in low+high+scale+offset)):
        raise AssetError('Invalid authored player collider')
    radii=[a*b for a,b in zip(high,scale)]
    settings['FeetOffset']=[offset[i] for i in (0,2,1)]
    settings['FeetRadii']=[radii[i] for i in (0,2,1)]
    extra.extend(settings['FeetOffset']+settings['FeetRadii'])
    scalars=[settings[k] for k in MOVEMENT_FIELDS+ANIMATION_FIELDS+PLAYER_FIELDS]
    payload = b'DNP1' + struct.pack('<II', 2, len(scalars)+len(extra))
    payload += struct.pack('<' + 'f' * (len(scalars)+len(extra)), *scalars, *extra)
    return settings, clips, payload
