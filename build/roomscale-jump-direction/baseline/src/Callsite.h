// Callsite.h -- find the game DLL's render code by asking it, not by guessing.
//
// The culling we want to change lives in tomb4/5/6.dll, which ship without
// PDBs: 1791 unnamed functions in TR4 alone. Searching that statically for a
// portal/frustum test is a long shot.
//
// But the DLL has to call across into the engine to draw anything, and we are
// already sitting on those calls. The return address inside our vid_setPass and
// ogl_drawVB detours IS a code address inside the DLL -- so one gameplay frame
// hands us the exact RVAs of the functions that submit geometry. From there the
// call graph leads back to whatever decided what to submit.
//
// Cheap, exact, and it turns a blind search into a starting point.
#pragma once

#include <cstdint>

namespace tr {

// Record a return address seen at one of our hooks. `tag` names the hook.
// Deduplicated; only new sites are logged, and only while the census is armed.
void NoteCallsite(const char* tag, void* returnAddress);

// Stop recording after this many distinct sites per tag, so a long session
// cannot fill the log.
void CallsiteCensusReset();

} // namespace tr
