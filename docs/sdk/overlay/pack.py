"""Package a BoBo app for the App Store.

usage: python <SDK>/pack.py <app dir> [--out <dir>] [--targets s3,s31,sf57] [--no-check]

Builds the ELF for each target (when app.json names one) and writes
<out>/<id>-<version>.zip laid out the way the store expects:

    app.json, mcp.json, ui.json   as written (ui.json: the file app.json "ui" names)
    icon-40.bin, icon-56.bin      from icon.png (S3 / Plus launcher sizes)
    elf/s3/app.elf, elf/s31/app.elf, elf/sf32lb57/app.elf
    files/<path>                  data files, installed as <path> in the app folder
                                  (the same files build.py copies for side-loading)

The store's limits are checked here too (bobo-cloud src/lib/app-check): the zip
and each board's download at most 2 MB, app.elf at most 512 KB, at most 16
data files (build.py MAX_DATA_FILES); then tools/pkgcheck.py runs the store's
upload check on the zip (skip it with --no-check). Upload the zip on the BoBo
website (开发者 → 上传新应用) or with `bobo publish`; or install it locally
without the store: copy it to the TF card's /apps-import/ (or `bobo import`)
and install it on the device under 应用商店 › 导入 (docs/spec.md §11).
"""
import argparse
import json
import subprocess
import sys
import zipfile
from pathlib import Path

SDK = Path(__file__).resolve().parent
sys.path.insert(0, str(SDK))
import build  # noqa: E402  (same folder)

MAX_PACKAGE = 2 * 1024 * 1024   # the upload
MAX_INSTALL = 2 * 1024 * 1024   # one board's download (firmware AppStore.cpp kMaxAppBytes)
MAX_ELF = 512 * 1024            # firmware ElfAppActivity.cpp kMaxElfBytes


def kb(n):
    return f'{(n + 1023) // 1024} KB'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('app')
    ap.add_argument('--out', default=None)
    ap.add_argument('--targets', default='s3,s31,sf57')
    ap.add_argument('--no-check', action='store_true', help='skip the store check (tools/pkgcheck.py)')
    args = ap.parse_args()

    app = Path(args.app).resolve()
    manifest = json.loads((app / 'app.json').read_text(encoding='utf-8'))
    out = Path(args.out).resolve() if args.out else app / 'dist'
    out.mkdir(parents=True, exist_ok=True)
    archive = out / f"{manifest['id']}-{manifest['version']}.zip"
    targets = [t for t in args.targets.split(',') if t]
    for t in targets:
        if t not in build.TARGETS:
            sys.exit(f'unknown target {t} (s3, s31, sf32lb57)')

    entries = {}
    for name in build.manifest_files(app, manifest):
        entries[name] = (app / name).read_bytes()
    if (app / 'icon.png').exists():
        for target in ('s3', 's31', 'sf57'):
            size = build.TARGETS[target]['icon']
            icon = out / 'build' / f'icon-{size}.bin'
            icon.parent.mkdir(parents=True, exist_ok=True)
            build.icon_bin(app / 'icon.png', size, icon)
            entries[f'icon-{size}.bin'] = icon.read_bytes()
    data = build.data_files(app, manifest)
    for rel, src in data:
        entries[f'files/{rel}'] = src.read_bytes()
    if manifest.get('elf'):
        for target in targets:
            subprocess.run([sys.executable, str(SDK / 'build.py'), str(app), '--target', target, '--out', str(out)],
                           check=True)
            arch = build.TARGETS[target].get('arch', target)
            elf = (out / target / manifest['id'] / 'app.elf').read_bytes()
            if len(elf) > MAX_ELF:
                sys.exit(f'elf/{target}/app.elf is {kb(len(elf))}; the device loads at most 512 KB')
            entries[f'elf/{arch}/app.elf'] = elf

    # What each board downloads: the shared files, its icon and its ELF.
    shared = sum(len(v) for k, v in entries.items() if not k.startswith(('elf/', 'icon-')))
    for target in ('s3', 's31', 'sf57'):
        arch = build.TARGETS[target].get('arch', target)
        icon = len(entries.get(f"icon-{build.TARGETS[target]['icon']}.bin", b''))
        elf = entries.get(f'elf/{arch}/app.elf')
        if manifest.get('elf') and elf is None:
            print(f'{target}: no ELF, boards with this chip cannot install the app')
            continue
        total = shared + icon + len(elf or b'')
        if total > MAX_INSTALL:
            sys.exit(f'{target}: the device would download {kb(total)}; the store allows at most 2 MB per board')
        print(f'{target}: {kb(total)} to download')

    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, blob in sorted(entries.items()):
            z.writestr(name, blob)
    if archive.stat().st_size > MAX_PACKAGE:
        sys.exit(f'{archive} is {kb(archive.stat().st_size)}; the store accepts packages up to 2 MB')
    print(f'-> {archive} ({len(entries)} files{f", {len(data)} data files" if data else ""}, {kb(archive.stat().st_size)})')
    if not args.no_check:
        sys.path.insert(0, str(SDK / 'tools'))
        import pkgcheck  # noqa: E402  (SDK tools/)
        report = pkgcheck.check_zip(archive)
        pkgcheck.print_report(report)
        print('local install: copy it to the TF card /apps-import/ (or bobo import), then 应用商店 › 导入 on the device')
        sys.exit(0 if report['ok'] else 1)


if __name__ == '__main__':
    main()
