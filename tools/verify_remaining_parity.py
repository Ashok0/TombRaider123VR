"""Verify TR1-3 hang, water-exit and sprint animation-state mappings.

Usage: python tools/verify_remaining_parity.py <installed-game-directory>
Reads installed PDP banks only; no modification to native assets.
"""
import struct
import sys
from pathlib import Path


def verify(path, game):
    if path.stem.upper().startswith("CUT"):
        return False
    data = path.read_bytes()
    count, = struct.unpack_from("<I", data)
    if count < 104:
        return False
    assert 4 + count * 32 <= len(data), (path, "animation bounds")

    def anim(index):
        start = 4 + index * 32
        return (struct.unpack_from("<h", data, start + 6)[0],
                *struct.unpack_from("<4h", data, start + 16))

    state, first, last, loop, _ = anim(96)
    assert state == 10 and loop == 96 and last - first + 1 == 22, (path, "native hang loop")
    states = {anim(i)[0] for i in range(count)}
    assert (58 if game == 1 else 55) in states, (path, "water-exit state")
    if game == 3:
        assert 73 in states, (path, "sprint state")
    return True


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    root = Path(sys.argv[1])
    for game in (1, 2, 3):
        count = sum(verify(path, game) for path in sorted((root / str(game)).rglob("*.PDP")))
        assert count, (game, "no Lara animation banks found")
        print(f"TR{game}: {count} hang/water-exit/sprint animation banks verified")
