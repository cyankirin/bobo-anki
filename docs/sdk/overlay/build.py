"""Build a BoBo ELF app and lay out its install folder.

usage: python app-sdk/build.py <app dir> [--target s3|s31 | --all] [--out <dir>]

<app dir> holds app.json, src/**/*.c and optionally mcp.json, ui.json (the
file app.json "ui" names), icon.png and files/ (data the app reads from its
folder: files/<path> is installed as <path>, at most one folder deep, the same
rules as the store package; see data_files()).
The result, <out>/<target>/<app id>/, is what goes on the SD card under the
apps folder (bobo.py push installs it; the store installs the same files).

The ELF is a position-independent shared object with app_main as its entry
(esp-iot-solution's project_elf()). It links against a stub of the firmware's
export table (host_symbols.json) and then libgcc, with -z defs: a call to
anything the firmware does not export fails here, at build time. The build
keeps the unstripped image as <out>/build/<target>/app.debug.elf (and a copy
under <out>/symbols/ named after the stripped app.elf's sha256) for
`bobo.py crash --symbolize`; app.elf is stripped. Then elfcheck.py runs the
device's own checks (CPU, relocation types, imports, API level).

Toolchains (bobo doctor shows what was found), first match wins:
  BOBO_TOOLCHAIN_S3 / BOBO_TOOLCHAIN_S31   the toolchain's bin folder
  PATH                                     xtensa-esp32s3-elf-gcc / riscv32-esp-elf-gcc
  PlatformIO packages                      ~/.platformio/packages/toolchain-*/bin
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

SDK = Path(__file__).resolve().parent
sys.path.insert(0, str(SDK / 'tools'))
import elfcheck  # noqa: E402  (app-sdk/tools)

EXE = '.exe' if os.name == 'nt' else ''
TARGETS = {
    's3': {
        'prefix': 'xtensa-esp32s3-elf-',
        'env': 'BOBO_TOOLCHAIN_S3',
        'package': 'toolchain-xtensa-esp-elf',
        'cores': ['platformio'],
        'cflags': ['-mlongcalls'],
        'strip': ['--remove-section=.xt.lit', '--remove-section=.xt.prop', '--remove-section=.xtensa.info'],
        'icon': 40,
    },
    's31': {
        'prefix': 'riscv32-esp-elf-',
        'env': 'BOBO_TOOLCHAIN_S31',
        'package': 'toolchain-riscv32-esp',
        'cores': ['pio31'],
        'cflags': ['-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f'],
        'strip': ['--remove-section=.riscv.attributes'],
        'icon': 56,
    },
    'sf57': {
        'prefix': 'arm-none-eabi-',
        'env': 'BOBO_TOOLCHAIN_SF57',
        'package': 'toolchain-arm-none-eabi',
        'cores': [],
        'cflags': ['-mcpu=cortex-m33', '-mthumb', '-mfloat-abi=hard', '-mfpu=fpv5-sp-d16',
                   '-mlong-calls', '-fno-plt', '-mno-unaligned-access'],
        'strip': ['--remove-section=.ARM.attributes'],
        'icon': 56,
        'arch': 'sf32lb57',
    },
}
# -g only feeds app.debug.elf: it does not change the generated code.
CFLAGS = ['-Os', '-g', '-std=gnu11', '-Wall', '-fPIC', '-fdata-sections', '-ffunction-sections', '-fvisibility=hidden',
          '-fno-common', '-Dmain=app_main']
LDFLAGS = ['-nostartfiles', '-nostdlib', '-shared', '-e', 'app_main', '-Wl,--gc-sections', '-Wl,--strip-discarded',
           '-Wl,-z,defs']
STRIP = ['--strip-unneeded', '--remove-section=.comment', '--remove-section=.got.loc', '--remove-section=.dynamic']
STUB = 'bobohost'

# Data files (files/): the device store's rules (bobo-cloud src/lib/app-check/layout.ts, firmware
# src/apps/AppStore.cpp safeRelativePath): one folder deep at most, 64 characters, safe characters,
# and never one of the app folder's own files.
MAX_DATA_FILES = 16  # each one is a catalog entry; the device reads at most 96 KB of catalog
SEGMENT = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]*$')
SYSTEM_FILES = {'app.json', 'app.elf', 'mcp.json', 'icon.bin', 'state.json', 'mcp-tools.json'}
JUNK = {'Thumbs.db', 'desktop.ini', '__pycache__'}


def toolchain_dirs(target):
    """Candidate bin folders for a target, in search order."""
    t = TARGETS[target]
    dirs = []
    if os.environ.get(t['env']):
        dirs.append(Path(os.environ[t['env']]))
    found = shutil.which(t['prefix'] + 'gcc')
    if found:
        dirs.append(Path(found).parent)
    home = Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home() / '.platformio'))
    dirs.append(home / 'packages' / t['package'] / 'bin')
    for parent in SDK.parents:  # a checkout next to dev-tools/
        for core in t['cores']:
            dirs.append(parent / 'dev-tools' / core / 'packages' / t['package'] / 'bin')
    return dirs


def find_toolchain(target):
    for d in toolchain_dirs(target):
        if (d / f"{TARGETS[target]['prefix']}gcc{EXE}").exists():
            return d
    return None


def tool(target, name):
    d = find_toolchain(target)
    if not d:
        sys.exit(f"{TARGETS[target]['prefix']}gcc not found for --target {target}: "
                 'run bobo doctor for how to install it')
    return str(d / f"{TARGETS[target]['prefix']}{name}{EXE}")


def manifest_files(app, manifest):
    """The JSON files an app ships as written: app.json, mcp.json and its ui.json."""
    names = ['app.json', 'mcp.json']
    ui = manifest.get('ui')
    if ui:
        if '/' in ui or '\\' in ui or '..' in ui:
            sys.exit(f'app.json "ui" must be a file name in the app folder: {ui}')
        if not (app / ui).exists():
            sys.exit(f'app.json names "ui": "{ui}" but the file is missing')
        json.loads((app / ui).read_text(encoding='utf-8'))  # fail the build on a malformed ui.json
        names.append(ui)
    return [n for n in names if (app / n).exists()]


def png_gray(path):
    """A non-interlaced PNG as (w, h, rows of 0..255 gray on white), standard library only:
    the fallback when Pillow is missing (Pillow scales icons a little better)."""
    import struct
    import zlib
    data = Path(path).read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        sys.exit(f'{path}: not a PNG file')
    pos, idat, palette, trns = 8, b'', None, None
    w = h = depth = ctype = interlace = 0
    while pos < len(data):
        n, kind = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE':
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif kind == b'tRNS':
            trns = body
        elif kind == b'IDAT':
            idat += body
        pos += 12 + n
    if interlace or depth not in (1, 2, 4, 8, 16):
        sys.exit(f'{path}: interlaced or {depth}-bit PNG: install Pillow (python -m pip install pillow)')
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, channels * depth // 8)
    stride = (w * channels * depth + 7) // 8
    raw, rows, prev = zlib.decompress(idat), [], bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b, c = prev[i], prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if depth < 8:
            per = 8 // depth
            vals = [(line[x // per] >> (8 - depth * (x % per + 1))) & ((1 << depth) - 1) for x in range(w * channels)]
        else:
            vals = [line[i] for i in range(0, len(line), depth // 8)]  # 16-bit: the high byte
        out = []
        scale = 255 // ((1 << depth) - 1) if depth < 8 and ctype != 3 else 1
        for x in range(w):
            if ctype == 3:
                r, g, bl = palette[vals[x]]
                alpha = trns[vals[x]] if trns and vals[x] < len(trns) else 255
            else:
                px = [v * scale for v in vals[x * channels:(x + 1) * channels]]
                r, g, bl = (px[0],) * 3 if ctype in (0, 4) else px[:3]
                alpha = px[-1] if ctype in (4, 6) else 255
            gray = (r * 299 + g * 587 + bl * 114) // 1000
            out.append((gray * alpha + 255 * (255 - alpha)) // 255)
        rows.append(out)
    return w, h, rows


def data_files(app, manifest):
    """files/ of the app as (path in the app folder, source file). Dot files and OS junk are skipped."""
    root = app / 'files'
    if not root.is_dir():
        return []
    ui = str(manifest.get('ui') or '').lower()
    out = []
    for src in sorted(root.rglob('*')):
        rel_parts = src.relative_to(root).parts
        if src.is_dir() or any(p.startswith('.') or p in JUNK for p in rel_parts):
            continue
        rel = '/'.join(rel_parts)
        if len(rel_parts) > 2 or len(rel) > 64 or not all(SEGMENT.match(p) for p in rel_parts):
            sys.exit(f'files/{rel}: on the device a data file is at most one folder deep and 64 characters, '
                     'made of A-Z a-z 0-9 . _ - and starting with a letter or digit')
        low = rel.lower()
        if low in SYSTEM_FILES or low == ui or rel_parts[0].lower() == 'user':
            sys.exit(f'files/{rel} would replace a file of the app folder '
                     '(app.json, app.elf, mcp.json, icon.bin, the ui file, state.json, user/)')
        out.append((rel, src))
    if len(out) > MAX_DATA_FILES:
        sys.exit(f'files/ holds {len(out)} files; the store installs at most {MAX_DATA_FILES}')
    return out


def icon_bin(png, size, out):
    """icon.png -> icon.bin (BOBOICN1, u16 w, u16 h, rows MSB-first, bit 0 = ink)."""
    try:
        from PIL import Image

        img = Image.open(png).convert('RGBA')
        bg = Image.new('RGBA', img.size, (255, 255, 255, 255))
        bg.paste(img, mask=img.split()[3])
        img = bg.convert('L').resize((size, size), Image.Resampling.LANCZOS)
        pix = img.load()

        def gray(x, y):
            return pix[x, y]
    except ImportError:  # box filter over the decoded PNG
        w, h, rows = png_gray(png)

        def gray(x, y):
            x0, x1 = x * w // size, max(x * w // size + 1, (x + 1) * w // size)
            y0, y1 = y * h // size, max(y * h // size + 1, (y + 1) * h // size)
            cells = [rows[j][i] for j in range(y0, y1) for i in range(x0, x1)]
            return sum(cells) // len(cells)
    data = bytearray(b'BOBOICN1' + size.to_bytes(2, 'little') + size.to_bytes(2, 'little'))
    for y in range(size):
        for xb in range(0, size, 8):
            byte = 0
            for b in range(8):
                x = xb + b
                white = not (x < size and gray(x, y) < 128)
                byte |= int(white) << (7 - b)
            data.append(byte)
    out.write_bytes(bytes(data))


def host_stub(target, build):
    """libbobohost.so: one empty function per exported symbol, so the linker resolves exactly
    the firmware's exports (and nothing else) as imports."""
    table = SDK / 'host_symbols.json'
    stub_dir = build / 'stub'
    lib = stub_dir / f'lib{STUB}.so'
    if lib.exists() and lib.stat().st_mtime >= table.stat().st_mtime:
        return stub_dir
    stub_dir.mkdir(parents=True, exist_ok=True)
    symbols = json.loads(table.read_text(encoding='utf-8'))['symbols']
    src = stub_dir / 'bobohost.c'
    src.write_text('/* Generated by app-sdk/build.py from host_symbols.json: link stub only, never loaded. */\n' +
                   ''.join(f'void {s["name"]}(void) {{}}\n' for s in symbols), encoding='utf-8')
    subprocess.run([tool(target, 'gcc'), *TARGETS[target]['cflags'], '-fPIC', '-fno-builtin', '-w', '-nostdlib',
                    '-shared', f'-Wl,-soname,lib{STUB}.so', str(src), '-o', str(lib)], check=True)
    return stub_dir


def build_elf(app, manifest, target, build):
    t = TARGETS[target]
    sources = sorted((app / 'src').rglob('*.c'))  # subfolders too (vendored libraries)
    if not sources:
        sys.exit('app.json names an elf but src/ has no .c files')
    stub = host_stub(target, build)
    debug = build / 'app.debug.elf'
    elf = build / 'app.elf'
    cmd = [tool(target, 'gcc'), *CFLAGS, *t['cflags'], f"-I{SDK / 'include'}", f"-I{app / 'src'}",
           *map(str, sources), *LDFLAGS, '-o', str(debug), f'-L{stub}', f'-l{STUB}', '-lgcc']
    if subprocess.run(cmd).returncode:
        sys.exit('build failed: an "undefined reference" names a function the firmware does not export '
                 '(app-sdk/docs/errors.md)')
    subprocess.run([tool(target, 'strip'), *STRIP, *t['strip'], '-o', str(elf), str(debug)], check=True)
    report = elfcheck.check(elf, target, manifest.get('api', 1))
    for e in report['errors']:
        print('ERROR', e, file=sys.stderr)
    if not report['ok']:
        sys.exit(f'{elf}: rejected by the device checks above')
    digest = hashlib.sha256(elf.read_bytes()).hexdigest()
    symbols = build.parent.parent / 'symbols'  # <out>/symbols/<target>-<sha>.debug.elf
    symbols.mkdir(parents=True, exist_ok=True)
    shutil.copy(debug, symbols / f'{target}-{digest[:16]}.debug.elf')
    kept = sorted(symbols.glob(f'{target}-*.debug.elf'), key=lambda p: p.stat().st_mtime)
    for old in kept[:-20]:
        old.unlink()
    (build / 'build.json').write_text(json.dumps({'id': manifest['id'], 'version': manifest.get('version', ''),
                                                  'target': target, 'sha256': digest,
                                                  'required_api': report['required_api']}, indent=1),
                                      encoding='utf-8')
    print(f"app.elf: {elf.stat().st_size} bytes, sha256 {digest[:16]}, needs API {report['required_api']} "
          f"(app.json: {manifest.get('api', 1)})")
    return elf


def build(app, target, out=None):
    """Builds one target; returns the install folder <out>/<target>/<id>/."""
    app = Path(app).resolve()
    manifest = json.loads((app / 'app.json').read_text(encoding='utf-8'))
    app_id = manifest['id']
    out = Path(out).resolve() if out else app / 'dist'
    dest = out / target / app_id
    if dest.exists():
        shutil.rmtree(dest)
    dest.mkdir(parents=True)
    build_dir = out / 'build' / target
    build_dir.mkdir(parents=True, exist_ok=True)
    if manifest.get('elf'):
        elf = build_elf(app, manifest, target, build_dir)
        shutil.copy(elf, dest / 'app.elf')
    for name in manifest_files(app, manifest):
        shutil.copy(app / name, dest / name)
    if (app / 'icon.png').exists():
        icon_bin(app / 'icon.png', TARGETS[target]['icon'], dest / 'icon.bin')
    for rel, src in data_files(app, manifest):  # data the app reads; the store package carries the same files
        (dest / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(src, dest / rel)
    print(f'-> {dest}')
    return dest


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('app')
    ap.add_argument('--target', choices=TARGETS, default='s3')
    ap.add_argument('--all', action='store_true', help='both targets (s3 and s31)')
    ap.add_argument('--out', default=None)
    args = ap.parse_args()
    for target in (TARGETS if args.all else [args.target]):
        build(args.app, target, args.out)


if __name__ == '__main__':
    main()
