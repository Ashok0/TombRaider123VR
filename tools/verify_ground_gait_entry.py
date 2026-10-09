"""Verify instant forward/side/back entry against installed TR1/2/3 animation tables.

Usage: python tools/verify_ground_gait_entry.py <game-directory>
Reads PDP files only. AnimateLara increments the standing frame before
GetChange selects the requested gait; no outgoing velocity is carried over.
"""
import struct
import sys
from pathlib import Path


def verify(path):
    if path.stem.upper().startswith("CUT"):
        return False  # Cutscene-only animation banks do not use Lara's gait table.
    data = path.read_bytes()
    tables, cursor = {}, 0
    for name, size in [("anims", 32), ("changes", 6), ("ranges", 8)]:
        count, = struct.unpack_from("<I", data, cursor)
        cursor += 4
        end = cursor + count * size
        assert end <= len(data), (path, name, "table bounds")
        tables[name] = data[cursor:end]
        cursor = end
    if len(tables["anims"]) < 104 * 32:
        return False  # Non-gameplay assets without Lara's base animations.

    def anim(index):
        record = tables["anims"][index * 32:(index + 1) * 32]
        return (struct.unpack_from("<h", record, 6)[0],
                *struct.unpack_from("<8h", record, 16))

    state, first, last, _, _, count, change, commands, _ = anim(11)
    assert state == 2 and first <= last and commands == 0, (path, "standing entry")
    for target in (1, 16, 21, 22):
        found = False
        for index in range(change, change + count):
            goal, ranges, start = struct.unpack_from("<3h", tables["changes"], index * 6)
            if goal != target:
                continue
            for index in range(start, start + ranges):
                low, high, dest, frame = struct.unpack_from("<4h", tables["ranges"], index * 8)
                if low <= first + 1 <= high:
                    dest_state, base, end, *_ = anim(dest)
                    velocity, acceleration = struct.unpack_from("<ii", tables["anims"], dest * 32 + 8)
                    speed = (velocity + acceleration * (frame - base)) >> 16
                    assert dest_state == target and base <= frame <= end, (path, target, "entry state")
                    assert 0 < speed * (1 if target == 1 else 3) <= 64, (path, target, "bounded first-tick speed", speed)
                    found = True
                    break
        assert found, (path, target, "cannot enter on first tick")
    # Ordinary animation whitelist used by PrepareGroundDirection.
    expected = {**dict.fromkeys((0, 6, 8, 10), 1),
                **dict.fromkeys((1, 2, 3, 4, 5, 7, 9, 20, 21), 0),
                **dict.fromkeys((11, 103), 2),
                **dict.fromkeys((38, 39, 40, 41), 16),
                65: 22, 66: 22, 67: 21, 68: 21}
    for index, expected_state in expected.items():
        assert anim(index)[0] == expected_state, (path, index, "unexpected gait state")
    state, first, last, jump, jump_frame, _, _, commands, _ = anim(41)
    assert state == 16 and last - first == 15 and commands == 0, (path, "backpedal startup")
    assert jump == 40 and anim(jump)[0] == 16, (path, "backpedal loop")
    frame, ticks = first, 0
    while frame <= last:
        frame = min(frame + 3, last) + 1
        ticks += 1
    assert ticks == 4, (path, "shortened startup duration")
    assert struct.unpack_from("<ii", tables["anims"], 41 * 32 + 8) == (2 << 16, 0)
    assert struct.unpack_from("<ii", tables["anims"], 40 * 32 + 8) == (10 << 16, 0)
    return True


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    root = Path(sys.argv[1])
    for game in ("1", "2", "3"):
        paths = sorted((root / game).rglob("*.PDP"))
        assert paths, (game, "no PDP files")
        count = sum(verify(path) for path in paths)
        assert count, (game, "no Lara animation tables")
        print(f"TR{game}: {count} tables verified: immediate forward/side/back entry and safe 4-tick backpedal startup")
