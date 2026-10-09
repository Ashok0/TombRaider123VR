"""Check the landing camera whitelist against installed TR1/2/3 animation tables.

Usage: python tools/verify_landing_camera.py <game-directory>
Reads PDP files only; covers grounded clip states, native jump/fall exits and
the TR1 water-exit versus TR2/3 landing use of animation 99.
"""
import struct
import sys
from pathlib import Path


def verify(path, game):
    if path.stem.upper().startswith("CUT"): return False
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
        return False

    def anim(index):
        record = tables["anims"][index * 32:(index + 1) * 32]
        return struct.unpack_from("<h", record, 6)[0], *struct.unpack_from("<8h", record, 16)

    expected = {13: 7, 24: 2, 31: 2, 82: 2, 92: 1}
    for index, state in expected.items():
        assert anim(index)[0] == state, (path, index, "landing/death state")
    state99 = anim(99)[0]
    assert state99 == (58 if game == "1" else 2), (path, 99, "water-exit/landing variant")
    assert anim(99)[3] == (101 if game == "1" else 82), (path, 99, "variant exit")
    for index, target in [(24, 11), (31, 11), (82, 11), (92, 0)]:
        assert anim(index)[3] == target, (path, index, "return to normal gait")

    # These airborne clips choose their grounded destinations through native
    # goal-state ranges. Inspect the transition tables, not guessed names.
    for source, target in [(23, 24), (28, 31), (75, 82), (77, 92)]:
        _, _, _, _, _, count, start, _, _ = anim(source)
        destinations = set()
        for index in range(start, start + count):
            goal, count, start_range = struct.unpack_from("<3h", tables["changes"], index * 6)
            if goal not in (1, 2):
                continue
            for index in range(start_range, start_range + count):
                _, _, dest, _ = struct.unpack_from("<4h", tables["ranges"], index * 8)
                destinations.add(dest)
        assert target in destinations, (path, source, target, "native landing transition")
    return True


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    root = Path(sys.argv[1])
    for game in ("1", "2", "3"):
        paths = sorted((root / game).rglob("*.PDP"))
        assert paths, (game, "no PDP files")
        count = sum(verify(path, game) for path in paths)
        assert count, (game, "no Lara animation tables")
        print(f"TR{game}: {count} landing animation tables verified")
