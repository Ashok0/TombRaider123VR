from pathlib import Path
import re
src=Path(r'C:\dev\TombRaider456VR\src\FirstPerson.cpp').read_text()
p=Path('src/FirstPerson.cpp'); s=p.read_text()
def section(a,b): return src[src.index(a):src.index(b,src.index(a))]
def adapt(t):
    for a,b in [('off::item_object','off::item_object_number'),('g_boundDll->game',"(g_boundDll->module[4]-L'1')"),('g_headingBase','g_heading.base'),('count>33','count>32'),('Frame result[33]','Frame result[32]'),('GunBasis controller','motiongun::Basis controller'),('const GunVec pole','const motiongun::Vec pole'),('CanTurnBody(item)','CanWalk(item)'),('g_autoBodyCalibration.pending','false')]: t=t.replace(a,b)
    t=re.sub(r'JointPoint\(item,([^,()]+),0,0,0\)',r'NativeJointPoint(item,\1)',t)
    t=t.replace('Radians(', 'locomotion::Radians(').replace('Wrap(', 'locomotion::Wrap(')
    t=t.replace('motiongun::DualWeapon(CurrentMotionWeapon())','DualMotionWeapon(CurrentMotionWeapon())')
    return t
s=s.replace('#include "FirstPersonIK.h"','#include "FirstPersonIK.h"\n#include "BodyFollower.h"\n#include "PistolAnimationBlend.h"')
globals=section('struct PistolArmPose {','void ResetMovementStabilization()')
globals=globals.replace('firstperson::BodyVisibilityRecovery g_bodyVisibility;','').replace('firstperson::AutoBodyCalibration g_autoBodyCalibration;','')
s=s.replace('double TurnTime() {',globals+'\nint CurrentMotionWeapon() { return g_boundDll && g_boundBase ? *Ptr<int16_t>(g_boundDll->lara+4) : 0; }\nbool PhysicalHolsterOwnsWeapon();\n\ndouble TurnTime() {')
# Native roomscale collision, now hands off the first unsupported step to gravity.
s=s.replace('coll.badPos = 384; coll.badNeg = -384; coll.badCeiling = 0;', 'coll.badPos = 32512; coll.badNeg = -384; coll.badCeiling = 0;')
s=s.replace('coll.flags = 5; // slopes are walls, lava is a pit (as lara_col_stop)', 'coll.flags = 1; // Slopes remain walls; native fall handles unsupported steps.')
s=s.replace('coll.floorSamples[0] < -384 || coll.floorSamples[0] > 384 ||','coll.floorSamples[0] < -384 ||')
s=s.replace('        if (!acceptedX && !acceptedZ) break;', '        if (coll.floorSamples[0]>384 || (!acceptedX && !acceptedZ)) break;')
s=s.replace('    const Vec requested = Rotate(DragRequest(pending,\n        Cfg().firstPersonRoomscaleDeadzoneMetres), g_heading.base) * scale;', '''    Vec request=DragRequest(pending,Cfg().firstPersonRoomscaleDeadzoneMetres);
    if(Cfg().firstPersonFullBodyIK) {
        const auto follow=g_roomscaleFollower.Request({pending.x,0,pending.z},Elapsed(g_roomscaleFollowTime),BodyFollowSettings());
        request={follow.x,follow.z};
    }
    const Vec requested=Rotate(request,g_heading.base)*scale;''')
s=s.replace('    const float delta = Wrap(g_heading.World(VR().HeadYawRadians()) - Radians(pos.y_rot));', '''    if(Cfg().firstPersonFullBodyIK && g_bodyFollower.valid) { pos.y_rot=Angle(g_bodyFollower.yaw); return; }
    const float delta = Wrap(g_heading.World(VR().HeadYawRadians()) - Radians(pos.y_rot));''')
# Keep floor/ceiling safety while allowing optional horizontal wall clipping.
s=s.replace('    if (g_blockCamera.collision) {\n        const auto& camera=g_blockCamera;', '    if (Cfg().firstPersonWallCollision && g_blockCamera.collision) {\n        const auto& camera=g_blockCamera;')
s=s.replace('    ClampHeadToCollision(item, body, renderedHead, eyeY,capsuleHeight);','    if(Cfg().firstPersonWallCollision) ClampHeadToCollision(item, body, renderedHead, eyeY,capsuleHeight);\n    else if(crouch || UseGroundRollCamera(item) || UseLandingCamera(item)) clampVertical(body[0],body[2]);')
# Port body solver at the palette boundary; native joints remain untouched.
ready='''bool FullBodyGripState(const uint8_t* item) {
    const int state=*reinterpret_cast<const int16_t*>(item+off::item_anim_state);
    return locomotion::IsLedgeHangState(state) || locomotion::IsLedgeMountState(state);
}
bool FullBodyIKReady() {
    if(!Cfg().firstPersonFullBodyIK || ControllerTrackingBlockedReason() || !Gate()) return false;
    const auto* item=*Ptr<uint8_t*>(g_boundDll->laraItem);
    const int status=*Ptr<int16_t>(g_boundDll->lara+off::lara_gun_status);
    if(!item || LaraWaterStatus()!=0 || *reinterpret_cast<const int16_t*>(item+off::item_hit_points)<=0) return false;
    if(!CanFitStandingBody(item) && !CanSteerCrouch(item) && !FullBodyGripState(item)) return false;
    if(status==1 && !FullBodyGripState(item)) return false;
    if(status!=0 && status!=1 && !MotionWeaponSupported(CurrentMotionWeapon())) return false;
    return true;
}
bool IsNativeAirborneBodyState(const uint8_t* item) {
    return locomotion::IsJumpOrFallState(*reinterpret_cast<const int16_t*>(item+off::item_anim_state));
}
motiongun::Vec BodyPaletteCameraOrigin(uint8_t* item,const motiongun::Frame& head) {
    if(g_boundDll->w2vMatrix) {
        const auto* w=Ptr<int32_t>(g_boundDll->w2vMatrix);
        if(w[0] || w[5] || w[10]) return {float(w[3]),float(w[7]),float(w[11])};
    }
    return motiongun::Sub(NativeJointPoint(item,14),head.origin);
}
'''
body=adapt(section('bool ApplyFullBodyIK(', '// Pose rendered arms'))
body=body.replace('const bool nativeWeaponArms=(weaponStatus==2 || weaponStatus==3) && !MotionReady();','const bool nativeWeaponArms=(weaponStatus==2 || weaponStatus==3) && !PhysicalHolsterOwnsWeapon();')
arm=adapt(section('bool ApplyArmIK(', '// Ladder eyes'))
arm=arm.replace('const bool armed=!shadow && MotionReady();','const bool armed=MotionReady();').replace('!UnarmedIKReady())','!UnarmedIKReadyForPass(shadow))')
# Actual view origin, including for non-pistol tracked arms, avoids integer wrist jitter.
start=arm.index('        if(armed && CurrentMotionWeapon()==1 && g_pistolBodyScope) {')
end=arm.index('        const float heading=',start)
arm=arm[:start]+'''        if(!shadow) {
            motiongun::Frame head{};
            if(ReadIKJoint(obj,joints,count,bones,mapping,poses,14,head))
                target.origin=motiongun::Sub(trackedOrigin,BodyPaletteCameraOrigin(item,head));
        }
'''+arm[end:]
helpers=adapt(section('void TraceArmGlance(', '#include "PhysicalHolster.inl"'))
a=s.index('bool ApplyArmIK(');b=s.index('void ApplyTrackedShadowArms(',a)
s=s[:a]+ready+helpers+body+arm+s[b:]
# Main and forearm draw scopes share exact solved frames in this render pass.
s=s.replace('            bits=UINT32_MAX & ~kHeadMeshBit & ~0x2400u;', '            g_pistolBodyScope=CurrentMotionWeapon()==1; g_pistolArmPose.valid=false;\n            bits=UINT32_MAX & ~kHeadMeshBit & ~0x2400u;')
s=s.replace('            g_bodySkinScope=g_bodySkinReady=false;bits=saved;\n            return;', '            g_bodySkinScope=g_bodySkinReady=false;bits=saved;g_pistolBodyScope=false;\n            return;')
s=s.replace('            bits=forearms;g_bodySkinScope=true;', '            g_pistolForearmScope=CurrentMotionWeapon()==1;\n            bits=forearms;g_bodySkinScope=true;')
s=s.replace('            g_bodySkinScope=g_bodySkinReady=false;bits=saved;\n        }', '            g_bodySkinScope=g_bodySkinReady=false;bits=saved;g_pistolForearmScope=false;\n        }')
a=s.index('                    if (MotionReady() || UnarmedIKReady()) {',s.index('void __cdecl Detour_GetJoints'))
b=s.index('                    g_bodySkinReady=true;',a)
s=s[:a]+'''                    const int pass=g_boundDll->renderPass ? *Ptr<int32_t>(g_boundDll->renderPass) : 0;
                    if(g_pistolBodyScope) { g_pistolArmPose.valid=false;g_pistolArmSolveReady=false; }
                    bool solved=g_pistolForearmScope && PistolBodyArmPose(item,g_bodyRenderPalette,count,pass,false);
                    if(!solved) solved=ApplyFullBodyIK(item,g_bodyRenderPalette,count);
                    if(!solved && (MotionReady() || UnarmedIKReady())) solved=ApplyArmIK(item,g_bodyRenderPalette,count);
                    if(solved) {
                        ++g_armIKSolved;
                        if(g_pistolBodyScope) PistolBodyArmPose(item,g_bodyRenderPalette,count,pass,true);
                        TraceArmGlance(item,g_bodyRenderPalette,count,pass);
                    }
'''+s[b:]
s=s.replace('    const motiongun::Frame desired{gun.basis,motiongun::Add(\n        wrist.origin,motiongun::Sub(gun.hand,gun.nativeHand))};','''    motiongun::Frame desired{gun.basis,motiongun::Add(
        wrist.origin,motiongun::Sub(gun.hand,gun.nativeHand))};
    const int pass=g_boundDll->renderPass ? *Ptr<int32_t>(g_boundDll->renderPass) : 0;
    if(PistolBodyPoseMatches(item,pass)) desired=g_pistolArmPose.joint[g_renderArm ? 2 : 5];''')
s=s.replace('    if (!g_haveHeading || !sameBody || relocated) {','    if (!g_haveHeading || !sameBody || relocated) {\n        ResetBodyFollower();')
s=s.replace('void FirstPersonToggle() {','void FirstPersonToggle() {\n    ResetBodyFollower();')
s=s.replace('void FirstPersonTrackingOriginChanged(float', 'void FirstPersonTrackingOriginChanged(float')
p.write_text(s)
