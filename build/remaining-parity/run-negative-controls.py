from pathlib import Path
import json
import subprocess
import sys

root=Path(__file__).resolve().parents[2]
cases=[
    ('mount_visibility','src/FirstPerson.cpp',
     'return IsHandsOnlyBodyState(item) ? firstperson::HandMeshBits : mask;',
     'return IsHandsOnlyBodyState(item) ? firstperson::HandMeshBits : UINT32_MAX;'),
    ('crouch_visibility','src/FirstPerson.cpp',
     'return item && (IsCrouchState(item) || firstperson::HideDirectionalJumpBody(',
     'return item && (false || firstperson::HideDirectionalJumpBody('),
    ('water_exit','src/LocomotionMath.h',
     'return game==1 ? state==58 : (game==2 || game==3) && state==55;',
     'return false;'),
    ('sprint','src/LocomotionMath.h',
     'if(state==73) return game==3;',
     'if(state==73) return false;'),
    ('pull_head_only','src/LedgePullGesture.h',
     'if (down<.22f || down+head-start.head<.22f)',
     'if (down+head-start.head<.22f)'),
    ('ledge_feedback','src/LedgeGrabFeedback.h',
     'x*x+z*z>=512.f*512.f',
     'x*x+z*z>=768.f*768.f'),
    ('holster_hover','src/GunHaptics.h',
     'hoverUntil[hand]=active ? now+100 : 0;',
     'hoverUntil[hand]=0;'),
    ('landing_camera','src/FirstPerson.cpp',
     'bool OrdinaryJumpLanding(const uint8_t* item) {\n    return g_active',
     'bool OrdinaryJumpLanding(const uint8_t* item) {\n    return false && g_active'),
    ('landing_torso','src/FirstPerson.cpp',
     '} else if(ordinaryLanding && g_jumpBodyObject==object && g_jumpBodyReference.valid) {',
     '} else if(false && ordinaryLanding && g_jumpBodyObject==object && g_jumpBodyReference.valid) {'),
    ('cuff_pixels','src/HandSkin.h',
     'vVisibleBodyWeight < 0.5 && !(vBodyWristWeight > 0.0 && vVisibleBodyWeight > 0.0)',
     'vVisibleBodyWeight < 0.5'),
]
if len(sys.argv)>1:
    cases=[case for case in cases if case[0] in sys.argv[1:]]
prior=root/'build/remaining-parity/negative-controls.json'
results=[result for result in json.loads(prior.read_text()) if result['case'] not in {case[0] for case in cases}] if prior.exists() else []
for name,file,old,new in cases:
    path=root/file;original=path.read_bytes()
    # Preserve source bytes, including pre-existing line endings.
    eol=b'\r\n' if b'\r\n' in original else b'\n'
    old=old.encode().replace(b'\n',eol);new=new.encode().replace(b'\n',eol)
    assert original.count(old)==1,(name,'unique mutation')
    try:
        path.write_bytes(original.replace(old,new))
        script='build_physics_selftest.cmd' if name=='cuff_pixels' else 'build_locomotion_selftest.cmd'
        run=subprocess.run(['cmd','/c',str(root/'tests'/script)],cwd=root,capture_output=True,text=True,timeout=180)
        log=run.stdout+run.stderr
        (root/'build/remaining-parity'/f'negative-{name}.log').write_text(log)
        failure=next((line for line in log.splitlines() if 'FAIL' in line),'NO ASSERTION FAILURE')
        results.append({'case':name,'exit':run.returncode,'failure':failure,'caught':run.returncode!=0 and 'FAIL' in log})
        print(json.dumps(results[-1]),flush=True)
    finally:
        path.write_bytes(original)
(root/'build/remaining-parity/negative-controls.json').write_text(json.dumps(results,indent=2)+'\n')
assert all(r['caught'] for r in results),'A regression did not detect its disabled correction'
