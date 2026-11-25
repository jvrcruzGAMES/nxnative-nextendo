#!/usr/bin/env python3
import os
import sys
import struct
import zlib
from pathlib import Path

def gob_off(x, y):
    return ((x % 64) // 32) * 256 + ((y % 8) // 2) * 64 + ((x % 32) // 16) * 32 + (y % 2) * 16 + (x % 16)

def nvn_bl_offset(x, y, z, row_bytes, rows, th, td=0):
    bh = 1 << th
    bd = 1 << td
    gobs_x = (row_bytes + 63) // 64
    blocks_y = (rows + 8 * bh - 1) // (8 * bh)
    block = 512 * bh * bd
    return ((z // bd) * gobs_x * blocks_y * block
          + (y // (8 * bh)) * gobs_x * block
          + (x // 64) * block
          + ((z % bd) * bh + (y % (8 * bh)) // 8) * 512
          + gob_off(x, y))

def make_png(width, height, rgba_data):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(rgba_data[y * width * 4 : (y + 1) * width * 4])
    idat = zlib.compress(bytes(raw))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat) + chunk(b'IEND', b'')

def extract_bntx(bntx_path, out_dir):
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    with open(bntx_path, 'rb') as f:
        data = f.read()

    num_tex = struct.unpack('<I', data[0x24:0x28])[0]
    tex_tbl_off = struct.unpack('<Q', data[0x28:0x30])[0]

    extracted = 0
    for i in range(num_tex):
        ptr = struct.unpack('<Q', data[tex_tbl_off + i*8:tex_tbl_off + (i+1)*8])[0]
        name_off = struct.unpack('<Q', data[ptr+0x60:ptr+0x68])[0]
        name_len = struct.unpack('<H', data[name_off:name_off+2])[0]
        name = data[name_off+2:name_off+2+name_len].decode('ascii')

        w = struct.unpack('<I', data[ptr+0x24:ptr+0x28])[0]
        h = struct.unpack('<I', data[ptr+0x28:ptr+0x2c])[0]
        blk_height_exp = struct.unpack('<I', data[ptr+0x34:ptr+0x38])[0]
        
        lvl_tbl_off = struct.unpack('<Q', data[ptr+0x70:ptr+0x78])[0]
        lvl0_off = struct.unpack('<Q', data[lvl_tbl_off:lvl_tbl_off+8])[0]

        bpp = 2
        row_bytes = w * bpp
        raw_pixels = bytearray(w * h * 4)
        src = data[lvl0_off:]

        for y in range(h):
            for x in range(w):
                src_byte_x = x * bpp
                bl_off = nvn_bl_offset(src_byte_x, y, 0, row_bytes, h, blk_height_exp)
                p0 = src[bl_off]
                p1 = src[bl_off + 1]
                idx = (y * w + x) * 4
                raw_pixels[idx + 0] = p0
                raw_pixels[idx + 1] = p0
                raw_pixels[idx + 2] = p0
                raw_pixels[idx + 3] = p1

        png = make_png(w, h, raw_pixels)
        out_file = out_dir / f"{name}.png"
        out_file.write_bytes(png)
        extracted += 1

    print(f"Extracted {extracted} textures from {bntx_path} to {out_dir}")

def main():
    if len(sys.argv) < 3:
        print("Usage: extract_mii_icons.py <bntx_path> <out_dir>")
        sys.exit(1)
    extract_bntx(sys.argv[1], sys.argv[2])

if __name__ == '__main__':
    main()
