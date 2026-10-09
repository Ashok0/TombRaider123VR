"""Verify priority-jump entry against every installed TR1/2/3 gameplay PDP.
Usage: python tools/verify_jump_priority.py <game-directory>
"""
import struct
import sys
from pathlib import Path


def verify(path):
    if path.stem.upper().startswith("CUT"):
        return False
    data, cursor, tables = path.read_bytes(), 0, {}
    for name, size in (("anims", 32), ("changes", 6), ("ranges", 8)):
        count, = struct.unpack_from("<I", data, cursor)
        cursor += 4
        end = cursor + count * size
        assert end <= len(data), (path, name, "bounds")
        tables[name], cursor = data[cursor:end], end
    if len(tables["anims"]) < 104 * 32:
        return False

    def anim(index):
        return (struct.unpack_from("<h", tables["anims"], index * 32 + 6)[0],
                *struct.unpack_from("<8h", tables["anims"], index * 32 + 16))

    def transitions(index, goal):
        record = anim(index)
        for change in range(record[6], record[6] + record[5]):
            state, count, start = struct.unpack_from("<3h", tables["changes"], change * 6)
            if state == goal:
                for entry in range(start, start + count):
                    yield struct.unpack_from("<4h", tables["ranges"], entry * 8)

    for index in (11, 103):
        state, first, last, *_ = anim(index)
        assert state == 2, (path, index, "standing state")
        frames = (first + 1,) if index == 11 else range(first + 1, last + 2)
        for frame in frames:
            matches = [(dest, at) for low, high, dest, at in transitions(index, 15) if low <= frame <= high]
            assert matches, (path, index, frame, "immediate compression entry")
            for dest, at in matches:
                state, base, end, *_ = anim(dest)
                assert state == 15 and base <= at <= end, (path, dest, "native compression")
        if index == 11:
            assert anim(index)[7] == 0, (path, "idle entry has no skipped commands")
            # Stop-to-idle may already be on its LAST entry frame when A is
            # sampled. TR3 increments past the dispatch window at that point.
            # Restarting the command-free entry before native control works
            # for every possible frame, not merely the first one.
            assert first == 185 and last in (185, 186), (path, "standing entry frame range")
            for current in range(first, last + 1):
                prepared = first
                assert any(low <= prepared + 1 <= high for low, high, _, _ in transitions(11, 15)), (path, current, "late entry jump")
            if last == 186:
                assert not any(low <= last + 1 <= high for low, high, _, _ in transitions(11, 15)), (path, "TR3 late-frame regression")
    for index, expected in ((2, 0), (3, 0), (8, 1), (10, 1)):
        state, first, last, dest, at, *_ = anim(index)
        assert state == expected and dest == 11 and at == 185, (path, index, "stopping handoff")
    state, first, last, *_ = anim(0)
    assert state == 1, (path, "run state")
    for frame in range(first + 1, last + 2):
        matches = [(dest, at) for low, high, dest, at in transitions(0, 3) if low <= frame <= high]
        assert matches, (path, frame, "running jump dispatch covers every stride frame")
        for dest, at in matches:
            state, base, end, *_ = anim(dest)
            assert state == 3 and base <= at <= end, (path, dest, "native running jump")
    for index in (6, 8, 10):
        assert anim(index)[0] == 1 and not list(transitions(index, 3)), (path, index, "run transition blocks jump")
    return True


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    for game in ("1", "2", "3"):
        count = sum(verify(p) for p in (Path(sys.argv[1]) / game).rglob("*.PDP"))
        assert count, (game, "no gameplay tables")
        print(f"TR{game}: {count} animation banks verify immediate compression and unchanged running jumps")
