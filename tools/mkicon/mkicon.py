#!/usr/bin/env python3
"""Write a WBTOOL .info icon: a disc (optionally with a play triangle).
usage: mkicon.py out.info stacksize [play|drawer]"""
import struct, sys, math

W, H, DEPTH = 46, 30, 2      # WB colours: 0 grey, 1 black, 2 white, 3 blue

def picture(play, selected):
    px = [[0] * W for _ in range(H)]
    cx, cy, rx, ry = 22.5, 14.5, 21.0, 13.5        # 2:1-ish pixels
    for y in range(H):
        for x in range(W):
            d = math.hypot((x - cx) / rx, (y - cy) / ry)
            if d <= 1.0:
                c = 1 if d > 0.92 else 2
                if 0.30 < d < 0.40: c = 3
                if d <= 0.16: c = 0
                elif d <= 0.22: c = 1
                # a shine band
                if c == 2 and 0.55 < d < 0.85 and -0.9 < (x - cx) / rx - (y - cy) / ry < -0.5:
                    c = 0
                px[y][x] = c
    if play:
        for y in range(H):
            for x in range(W):
                tx, ty = x - 30, y - 15
                if 0 <= tx <= 14 and abs(ty) <= (14 - tx) * 0.55 + 0.5:
                    edge = tx == 0 or abs(ty) > (14 - tx) * 0.55 - 1.0
                    px[y][x] = 1 if edge else 3
    if selected:
        px = [[{0: 1, 1: 2, 2: 3, 3: 0}[c] if c else 0 for c in r] for r in px]
    return px

def planes(px):
    bpr = ((W + 15) // 16) * 2
    out = b''
    for p in range(DEPTH):
        for y in range(H):
            row = bytearray(bpr)
            for x in range(W):
                if (px[y][x] >> p) & 1:
                    row[x // 8] |= 0x80 >> (x % 8)
            out += bytes(row)
    return out

def image(px):
    return struct.pack('>hhhhhIBBI', 0, 0, W, H, DEPTH, 1, 3, 0, 0) + planes(px)

def main():
    out, stack = sys.argv[1], int(sys.argv[2])
    play = len(sys.argv) > 3 and sys.argv[3] == 'play'
    drawer = len(sys.argv) > 3 and sys.argv[3] == 'drawer'
    gadget = struct.pack('>IhhhhHHHIIIIIHI',
                         0, 0, 0, W, H + 1,
                         0x0006,   # GADGIMAGE | GADGHIMAGE
                         0x0003, 0x0001,
                         1, 1, 0, 0, 0, 0,
                         1)        # UserData: OS 2 icon revision
    do = struct.pack('>HH', 0xE310, 1) + gadget + struct.pack(
        '>BBIIiiIII', 2 if drawer else 3, 0, 0, 0, -0x80000000, -0x80000000,
        1 if drawer else 0, 0, stack)
    assert len(do) == 78, len(do)
    data = do
    if drawer:
        # OldDrawerData: a NewWindow (48 bytes), dd_CurrentX/Y
        data += struct.pack('>hhhhbbIIIIIIIhhHHH', 60, 40, 420, 200, -1, -1,
                            0, 0, 0, 0, 0, 0, 0, 90, 40, 0xffff, 0xffff, 1)
        data += struct.pack('>ii', 0, 0)
    data += image(picture(play, False)) + image(picture(play, True))
    if drawer:
        data += struct.pack('>IH', 0, 0)    # DrawerData2: dd_Flags 0 (default), view by icon
    open(out, 'wb').write(data)

main()
