#!/usr/bin/env python3
"""Makes the Vita icon and LiveArea images from the art in your own Carbon game files, or checks your own images.

Writes sce_sys/game/, which git ignores and the build prefers over the plain defaults; --vpk puts them into a VPK.
Based on NFSUG2_VITA's tools/vita/livearea_art.py (MIT); Carbon's texture packs (TPK) have their own header layout.
The images are made from EA's art on the user's machine and never belong in the repository or a shared build.
"""

import argparse
import math
import os
import shutil
import struct
import sys
import zipfile
import zlib

HEADERS = 0x33310004
FORMATS = 0x33310005
PIXELS = 0x33320002
HEADER_SIZE = 0x58          # fixed part of a Carbon texture header; a length-prefixed name follows
FORMAT_SIZE = 24
# (file, texture): the first one found is used.
LOGOS = ((os.path.join('GLOBAL', 'UniqueBootTextures.bin'), 'NFS07_LOGO'),
         (os.path.join('GLOBAL', 'DEMO_MARKETING.BUN'), 'NFS07_LOGO'),
         (os.path.join('FRONTEND', 'DEMOFRONTB1.BUN'), 'NFS07_LOGO'))
ARTS = ((os.path.join('FRONTEND', 'DEMOFRONTB1.BUN'), 'SPLASH_SCREEN_WS_3'),)
GLOW_INNER = (0x10, 0x30, 0x24)
GLOW_OUTER = (0x02, 0x06, 0x05)


def chunks(data, start, end, found):
    """Collects (id, size, offset) of every chunk, nested ones included."""
    while start + 8 <= end:
        ident, size = struct.unpack_from('<II', data, start)
        if start + 8 + size > end:
            break
        found.append((ident, size, start + 8))
        if ident & 0x80000000:
            chunks(data, start + 8, start + 8 + size, found)
        start += 8 + size
    return found


def colour565(c):
    return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)


def decode(data, offset, width, height, fourcc):
    """DXT1/DXT3/DXT5 or A8R8G8B8 (D3DFMT 21) to a flat RGBA list."""
    if fourcc == b'\x15\0\0\0':
        return [(data[offset + i * 4 + 2], data[offset + i * 4 + 1], data[offset + i * 4], data[offset + i * 4 + 3])
                for i in range(width * height)]
    if fourcc not in (b'DXT1', b'DXT3', b'DXT5'):
        raise ValueError('unsupported texture format %r' % fourcc)
    pixels = [(0, 0, 0, 0)] * (width * height)
    position = offset
    for top in range(0, height, 4):
        for left in range(0, width, 4):
            alpha = 0
            if fourcc != b'DXT1':
                alpha = int.from_bytes(data[position:position + 8], 'little')
                position += 8
            c0, c1, bits = struct.unpack_from('<HHI', data, position)
            position += 8
            a, b = colour565(c0), colour565(c1)
            if fourcc == b'DXT1' and c0 <= c1:
                palette = (a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0))
            else:
                palette = (a + (255,), b + (255,), tuple((2 * x + y) // 3 for x, y in zip(a, b)) + (255,),
                           tuple((x + 2 * y) // 3 for x, y in zip(a, b)) + (255,))
            if fourcc == b'DXT5':
                a0, a1 = alpha & 255, (alpha >> 8) & 255
                if a0 > a1:
                    levels = [a0, a1] + [((6 - i) * a0 + (i + 1) * a1) // 7 for i in range(6)]
                else:
                    levels = [a0, a1] + [((4 - i) * a0 + (i + 1) * a1) // 5 for i in range(4)] + [0, 255]
            for i in range(16):
                x, y = left + i % 4, top + i // 4
                r, g, bb, al = palette[(bits >> (2 * i)) & 3]
                if fourcc == b'DXT3':
                    al = ((alpha >> (4 * i)) & 15) * 17
                elif fourcc == b'DXT5':
                    al = levels[(alpha >> (16 + 3 * i)) & 7]
                if x < width and y < height:
                    pixels[y * width + x] = (r, g, bb, al)
    return pixels


def load_texture(path, name):
    """Returns (width, height, RGBA pixels) of a named texture (top mip)."""
    with open(path, 'rb') as f:
        data = f.read()
    found = chunks(data, 0, len(data), [])
    packs = zip([c for c in found if c[0] == HEADERS], [c for c in found if c[0] == FORMATS],
                [c for c in found if c[0] == PIXELS])
    for headers, formats, pixels in packs:
        base = pixels[2]
        while base % 0x80 and data[base] == 0x11:
            base += 1
        position, end, index = headers[2], headers[2] + headers[1], 0
        while position + HEADER_SIZE < end:
            length = data[position + HEADER_SIZE]
            found_name = data[position + HEADER_SIZE + 1:position + HEADER_SIZE + length].split(b'\0')[0]
            if found_name.decode('latin1').strip() == name:
                placement, = struct.unpack_from('<I', data, position + 0x14)
                width, height = struct.unpack_from('<2H', data, position + 0x28)
                fourcc = data[formats[2] + index * FORMAT_SIZE + 12:formats[2] + index * FORMAT_SIZE + 16]
                return width, height, decode(data, base + placement, width, height, fourcc)
            position, index = position + HEADER_SIZE + 1 + length, index + 1
    raise ValueError('%s not found in %s' % (name, path))


def first_texture(game, candidates):
    problems = []
    for name, texture in candidates:
        path = os.path.join(game, name)
        if not os.path.isfile(path):
            problems.append('%s missing' % name)
            continue
        try:
            return load_texture(path, texture)
        except (ValueError, struct.error, IndexError) as error:
            problems.append(str(error))
    raise ValueError('; '.join(problems))


def trim(width, height, pixels):
    """Crops away fully transparent borders."""
    rows = [y for y in range(height) if any(pixels[y * width + x][3] for x in range(width))]
    columns = [x for x in range(width) if any(pixels[y * width + x][3] for y in range(height))]
    left, right, top, bottom = columns[0], columns[-1] + 1, rows[0], rows[-1] + 1
    return right - left, bottom - top, [pixels[y * width + x] for y in range(top, bottom) for x in range(left, right)]


def weights(source, target):
    """Per output position: (first source index, normalised triangle weights)."""
    scale = source / target
    support = max(1.0, scale)
    table = []
    for i in range(target):
        centre = (i + 0.5) * scale
        first = max(0, int(math.floor(centre - support)))
        last = min(source, int(math.ceil(centre + support)))
        w = [max(0.0, 1.0 - abs((j + 0.5 - centre) / support)) for j in range(first, last)]
        total = sum(w) or 1.0
        table.append((first, [x / total for x in w]))
    return table


def resize(width, height, pixels, new_width, new_height):
    """Smooth resize on premultiplied alpha."""
    pre = [(r * a / 255.0, g * a / 255.0, b * a / 255.0, float(a)) for r, g, b, a in pixels]
    across = weights(width, new_width)
    rows = []
    for y in range(height):
        row = pre[y * width:(y + 1) * width]
        rows.append([tuple(sum(row[first + k][c] * w for k, w in enumerate(ws)) for c in range(4))
                     for first, ws in across])
    out = []
    for first, ws in weights(height, new_height):
        for x in range(new_width):
            r, g, b, a = (sum(rows[first + k][x][c] * w for k, w in enumerate(ws)) for c in range(4))
            out.append((r * 255.0 / a, g * 255.0 / a, b * 255.0 / a, a) if a > 0 else (0.0, 0.0, 0.0, 0.0))
    return out


def fit(width, height, max_width, max_height):
    """Largest size inside the box that keeps the proportions."""
    scale = min(max_width / width, max_height / height)
    return max(1, round(width * scale)), max(1, round(height * scale))


def quantise(pixels, limit=256):
    """Median cut to a palette; returns (palette, indices)."""
    counts = {}
    for p in pixels:
        key = tuple(min(255, max(0, int(round(c)))) for c in p)
        counts[key] = counts.get(key, 0) + 1
    boxes = [list(counts.items())]
    while len(boxes) < limit:
        best, channel, spread = None, 0, 0
        for box in boxes:
            if len(box) < 2:
                continue
            for c in range(4):
                values = [colour[c] for colour, _ in box]
                size = (max(values) - min(values)) * math.sqrt(sum(n for _, n in box))
                if size > spread:
                    best, channel, spread = box, c, size
        if best is None:
            break
        best.sort(key=lambda item: item[0][channel])
        half, running, cut = sum(n for _, n in best) / 2.0, 0, 1
        for i, (_, n) in enumerate(best[:-1]):
            running += n
            cut = i + 1
            if running >= half:
                break
        boxes.remove(best)
        boxes += [best[:cut], best[cut:]]
    palette, lookup = [], {}
    for box in boxes:
        total = sum(n for _, n in box)
        lookup.update((colour, len(palette)) for colour, _ in box)
        palette.append(tuple(int(round(sum(colour[c] * n for colour, n in box) / total)) for c in range(4)))
    return palette, [lookup[tuple(min(255, max(0, int(round(c)))) for c in p)] for p in pixels]


def write_png(path, width, height, palette, indices):
    """Writes an 8-bit palette PNG, as the Vita requires."""
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    rows = b''.join(b'\0' + bytes(indices[y * width:(y + 1) * width]) for y in range(height))
    parts = [b'\x89PNG\r\n\x1a\n', chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 3, 0, 0, 0)),
             chunk(b'PLTE', b''.join(bytes(c[:3]) for c in palette))]
    if any(c[3] < 255 for c in palette):
        parts.append(chunk(b'tRNS', bytes(c[3] for c in palette)))
    parts += [chunk(b'IDAT', zlib.compress(rows, 9)), chunk(b'IEND', b'')]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(b''.join(parts))


def check_image(path, width, height):
    """Returns why the Vita would reject this image, or None."""
    try:
        with open(path, 'rb') as f:
            head = f.read(26)
    except OSError:
        return 'is missing'
    if len(head) < 26 or head[:8] != b'\x89PNG\r\n\x1a\n':
        return 'is not a PNG file'
    found_width, found_height, depth, colour = struct.unpack('>IIBB', head[16:26])
    if (found_width, found_height) != (width, height):
        return 'is %dx%d, it must be %dx%d' % (found_width, found_height, width, height)
    if depth != 8 or colour != 3:
        return 'must be an 8-bit palette PNG (256 colours or fewer), as pngquant makes'
    return None


def copy_images(source, out):
    """Checks a folder of your own images and copies them."""
    for name, width, height in IMAGES:
        path = os.path.join(source, name)
        if not os.path.isfile(path):
            path = os.path.join(source, os.path.basename(name))
        problem = check_image(path, width, height)
        if problem:
            sys.exit('%s %s.' % (os.path.join(source, os.path.basename(name)), problem))
        os.makedirs(os.path.dirname(os.path.join(out, name)), exist_ok=True)
        if os.path.abspath(path) != os.path.abspath(os.path.join(out, name)):
            shutil.copyfile(path, os.path.join(out, name))


def tight(width, height, pixels, threshold=96):
    """Crops to the part whose alpha reaches threshold (drops the faint outer glow), keeping a small margin."""
    rows = [y for y in range(height) if any(pixels[y * width + x][3] >= threshold for x in range(width))]
    columns = [x for x in range(width) if any(pixels[y * width + x][3] >= threshold for y in range(height))]
    left, right = max(0, columns[0] - 3), min(width, columns[-1] + 4)
    top, bottom = max(0, rows[0] - 3), min(height, rows[-1] + 4)
    return right - left, bottom - top, [pixels[y * width + x] for y in range(top, bottom) for x in range(left, right)]


def cover(width, height, pixels, new_width, new_height, focus_x=0.5, focus_y=0.5, dim=1.0):
    """Scales an opaque image to fill the box and crops around the focus point; dim < 1 darkens it."""
    scale = max(new_width / width, new_height / height)
    scaled_width, scaled_height = max(new_width, round(width * scale)), max(new_height, round(height * scale))
    scaled = resize(width, height, pixels, scaled_width, scaled_height)
    left = min(scaled_width - new_width, max(0, round(scaled_width * focus_x - new_width / 2)))
    top = min(scaled_height - new_height, max(0, round(scaled_height * focus_y - new_height / 2)))
    return [tuple(c * dim for c in scaled[y * scaled_width + x][:3]) + (255.0,)
            for y in range(top, top + new_height) for x in range(left, left + new_width)]


def over(canvas, canvas_width, canvas_height, width, height, pixels):
    """Draws an RGBA image centred over an opaque canvas."""
    left, top = (canvas_width - width) // 2, (canvas_height - height) // 2
    out = list(canvas)
    for y in range(height):
        for x in range(width):
            r, g, b, a = pixels[y * width + x]
            t = a / 255.0
            i = (top + y) * canvas_width + left + x
            br, bg, bb, _ = out[i]
            out[i] = (r * t + br * (1 - t), g * t + bg * (1 - t), b * t + bb * (1 - t), 255.0)
    return out


def placed(canvas_width, canvas_height, width, height, pixels):
    """An RGBA image centred on a fully transparent canvas."""
    out = [(0.0, 0.0, 0.0, 0.0)] * (canvas_width * canvas_height)
    left, top = (canvas_width - width) // 2, (canvas_height - height) // 2
    for y in range(height):
        out[(top + y) * canvas_width + left:(top + y) * canvas_width + left + width] = pixels[y * width:(y + 1) * width]
    return out


def glow(width, height):
    """Plain dark green background (no art found), as RGBA."""
    radius = max(width, height) / 2.0
    out = []
    for y in range(height):
        for x in range(width):
            t = min(1.0, math.hypot(x + 0.5 - width / 2.0, y + 0.5 - height / 2.0) / radius)
            out.append(tuple(a + (b - a) * t for a, b in zip(GLOW_INNER, GLOW_OUTER)) + (255.0,))
    return out


IMAGES = (('icon0.png', 128, 128), (os.path.join('livearea', 'contents', 'bg.png'), 840, 500),
          (os.path.join('livearea', 'contents', 'startup.png'), 280, 158))


def from_game(game, out):
    """Makes the three images from the game's logo and splash art."""
    if not os.path.isfile(os.path.join(game, 'NFSC.exe')):
        sys.exit('%s has no NFSC.exe: give the game folder.' % game)
    try:
        logo = trim(*first_texture(game, LOGOS))
    except ValueError as error:
        sys.exit('Could not read the logo (%s). The build will use the plain default images.' % error)
    try:
        art = first_texture(game, ARTS)

        def backdrop(width, height, focus_x, focus_y, dim):
            return cover(*art, width, height, focus_x, focus_y, dim)
    except ValueError as error:
        print('No splash art (%s): plain background.' % error)

        def backdrop(width, height, focus_x, focus_y, dim):
            return glow(width, height)
    contents = os.path.join(out, 'livearea', 'contents')
    small = tight(*logo)
    size = fit(small[0], small[1], 124, 124)
    icon = over(backdrop(128, 128, 0.72, 0.45, 0.45), 128, 128, size[0], size[1], resize(*small, *size))
    write_png(os.path.join(out, 'icon0.png'), 128, 128, *quantise(icon))
    # The gate (launch button) is the logo alone on a transparent background: the LiveArea art shows through.
    size = fit(logo[0], logo[1], 276, 154)
    startup = placed(280, 158, size[0], size[1], resize(*logo, *size))
    write_png(os.path.join(contents, 'startup.png'), 280, 158, *quantise(startup))
    write_png(os.path.join(contents, 'bg.png'), 840, 500, *quantise(backdrop(840, 500, 0.5, 0.5, 1.0)))


def patch_vpk(vpk, out):
    """Replaces the three images inside a VPK, leaving the rest."""
    wanted = {'sce_sys/' + name.replace(os.sep, '/'): os.path.join(out, name) for name, _, _ in IMAGES}
    temporary = vpk + '.new'
    if not zipfile.is_zipfile(vpk):
        sys.exit('%s is not a VPK file.' % vpk)
    with zipfile.ZipFile(vpk) as old, zipfile.ZipFile(temporary, 'w', zipfile.ZIP_DEFLATED) as new:
        missing = set(wanted) - set(old.namelist())
        if missing or 'eboot.bin' not in old.namelist():
            os.remove(temporary)
            sys.exit('%s does not look like a NFSCVita VPK.' % vpk)
        for item in old.infolist():
            if item.filename in wanted:
                new.write(wanted[item.filename], item.filename)
            else:
                new.writestr(item, old.read(item.filename))
    os.replace(temporary, vpk)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('game', nargs='?', help='the game folder, the one with NFSC.exe')
    parser.add_argument('--images', help='use your own icon0.png, bg.png and startup.png from this folder instead')
    parser.add_argument('--out', default=os.path.join(here, '..', '..', 'sce_sys', 'game'),
                        help='output folder (default: sce_sys/game)')
    parser.add_argument('--vpk', help='also put the images into this VPK, so it need not be built again')
    args = parser.parse_args()
    if bool(args.game) == bool(args.images):
        parser.error('give the game folder, or --images with a folder of your own images')
    out = os.path.normpath(args.out)
    if args.images:
        copy_images(args.images, out)
    else:
        from_game(args.game, out)
    print('Wrote icon0.png, livearea/contents/startup.png and bg.png to ' + out)
    if args.vpk:
        patch_vpk(args.vpk, out)
        print('Put them into ' + args.vpk)


if __name__ == '__main__':
    main()
