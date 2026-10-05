"""Verify TR1/2/3 jump-roll animation IDs against installed PDP tables.

Usage: python tools/verify_jump_roll.py <game-directory>
"""
import struct
import sys
from pathlib import Path


def verify(path, game):
    if path.stem.upper().startswith("CUT"):
        return False
    data, cursor, tables = path.read_bytes(), 0, {}
    for name, size in [("anims", 32), ("changes", 6), ("ranges", 8), ("commands", 2)]:
        count, = struct.unpack_from("<I", data, cursor)
        cursor += 4
        end = cursor + count * size
        assert end <= len(data), (path, name, "bounds")
        tables[name], cursor = data[cursor:end], end
    flips = ([(113, 3, 160), (43, 3, 47), (48, 25, 49)] if game == "1"
             else [(207, 3, 209), (210, 3, 211), (212, 25, 213)])
    if len(tables["anims"]) < (1 + max(max(index, dest) for index, _, dest in flips)) * 32:
        return False
    for index, state, destination in flips:
        record = tables["anims"][index * 32:(index + 1) * 32]
        actual_state, = struct.unpack_from("<h", record, 6)
        first, last, jump, _, _, _, count, offset = struct.unpack_from("<8h", record, 16)
        assert actual_state == state and jump == destination, (path, index, "jump-roll state/exit")
        assert count == 1, (path, index, "one animation command")
        command, frame, effect = struct.unpack_from("<3h", tables["commands"], offset * 2)
        assert command == 6 and effect == 0 and first <= frame <= last, (path, index, "turn180 effect")
    return True


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    for game in ("1", "2", "3"):
        paths = sorted((Path(sys.argv[1]) / game).rglob("*.PDP"))
        count = sum(verify(path, game) for path in paths)
        assert count, (game, "no Lara animation tables")
        animations = "113/43/48" if game == "1" else "207/210/212"
        print(f"TR{game}: {count} tables verified: jump rolls {animations} each issue one turn180 effect")
