"""Check a BoBo store package (the zip `bobo pack` makes) the way the store's upload check does.

usage: python tools/pkgcheck.py <package.zip> [--json] [--board metalio_eink4|metalio_eink4plus]

A port of the BoBo website's validator (bobo-cloud src/lib/app-check/package.ts, the same issue
codes as /api/apps/check and the docs' 报错表) using only the Python standard library, plus the
device loader's ELF checks (elfcheck.py). The device's 应用商店 › 导入 runs the same rules for its
own board before it installs a package (firmware src/apps/AppImport.cpp). What only the website
knows is not checked here: whether the id belongs to another developer (slug_taken) and whether
the version is newer than the published one (version_not_newer).

Exit code 0 when no issue is an error. --json prints the report: {ok, app, boards[], issues[]}.
"""
import argparse
import io
import json
import re
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import elfcheck  # noqa: E402

LIMITS = {'package': 2 * 1024 * 1024, 'install': 2 * 1024 * 1024, 'elf': 512 * 1024, 'manifest': 8 * 1024,
          'ui': 64 * 1024, 'mcp': 32 * 1024, 'icon': 1024, 'data_files': 16, 'device_path': 64, 'zip_entries': 200,
          'unzipped': 8 * 1024 * 1024}
BOARDS = {
    'metalio_eink4': {'arch': 's3', 'icon': 40, 'label': '3.97 寸'},
    'metalio_eink4plus': {'arch': 's31', 'icon': 56, 'label': '4.7 寸 Plus'},
    'sifli_gyd7n6': {'arch': 'sf32lb57', 'icon': 56, 'label': '4.7 寸 思澈'},
}
# bobo-cloud src/lib/app-check/reserved.ts (firmware AppRegistry.cpp kBuiltins + website routes).
RESERVED = {'weread', 'boboread', 'journal', 'memory', 'capsules', 'pomodoro', 'weather', 'huabao', 'moments',
            'assistant', 'transfer', 'store', 'store-plugin', 'bobo-store', 'new', 'docs', 'admin', 'api', 'mcp',
            'sdk'}
SAFE_PATH = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,63}(/[A-Za-z0-9][A-Za-z0-9._-]{0,63}){0,3}$')
APP_ID = re.compile(r'^[a-z0-9][a-z0-9-]{1,38}[a-z0-9]$')
SEMVER = re.compile(r'^\d+\.\d+\.\d+$')
SEGMENT = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]*$')
ELF_ENTRY = re.compile(r'^elf/(s3|s31|sf32lb57)/app\.elf$')
ICON = re.compile(r'^icon-(\d+)\.bin$')
IGNORED = re.compile(r'(^|/)(__MACOSX/|\.DS_Store$|\._[^/]*$|Thumbs\.db$|desktop\.ini$)')
SYSTEM_FILES = {'app.json', 'app.elf', 'mcp.json', 'icon.bin', 'state.json', 'mcp-tools.json'}


class Report:
    def __init__(self):
        self.issues = []

    def add(self, level, code, message, file=None):
        item = {'level': level, 'code': code, 'message': message}
        if file:
            item['file'] = file
        self.issues.append(item)

    def errors(self):
        return [i for i in self.issues if i['level'] == 'error']


def read_zip(data):
    """{path: bytes} without folders and OS junk, the common root folder stripped. Raises ValueError."""
    try:
        z = zipfile.ZipFile(io.BytesIO(data))
    except zipfile.BadZipFile as e:
        raise ValueError(f'无法读取 zip：{e}')
    files, total = {}, 0
    for info in z.infolist():
        name = info.filename.replace('\\', '/')
        if name.endswith('/') or IGNORED.search(name):
            continue
        if len(files) >= LIMITS['zip_entries']:
            raise ValueError(f"文件数量超过 {LIMITS['zip_entries']}")
        total += info.file_size
        if total > LIMITS['unzipped']:
            raise ValueError('解压后超过 8 MB')
        if info.flag_bits & 0x1:
            raise ValueError('zip 加密了')
        files[name] = z.read(info)
    if files:
        first = next(iter(files)).split('/')[0]
        if all(p.startswith(first + '/') for p in files):
            files = {p[len(first) + 1:]: d for p, d in files.items()}
    return files


def parse_json(data):
    try:
        value = json.loads(data.decode('utf-8-sig'))
    except (UnicodeDecodeError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def ui_file_name(manifest):
    ui = manifest.get('ui') if isinstance(manifest.get('ui'), str) else ''
    ui = ui.strip()
    return ui if ui and SEGMENT.match(ui) and '..' not in ui and len(ui) <= 64 else None


def data_file_path(source, manifest):
    """files/<rel> -> (<rel>, None) or (None, 'path'|'reserved') (layout.ts dataFilePath)."""
    rel = source[len('files/'):]
    parts = rel.split('/')
    if not rel or len(parts) > 2 or len(rel) > 64 or not all(SEGMENT.match(p) for p in parts) or '..' in rel:
        return None, 'path'
    low = rel.lower()
    ui = (ui_file_name(manifest) or '').lower()
    if low in SYSTEM_FILES or (ui and low == ui) or parts[0].lower() == 'user' or low.startswith('.new-'):
        return None, 'reserved'
    return rel, None


def board_delivery(manifest, paths, board):
    """[(source, installed path)], missing reason (layout.ts boardDelivery)."""
    target = BOARDS[board]
    out, taken = [], set()

    def add(source, path):
        if source not in paths or path in taken:
            return False
        out.append((source, path))
        taken.add(path)
        return True

    add('app.json', 'app.json')
    add('mcp.json', 'mcp.json')
    ui = ui_file_name(manifest)
    add(ui, ui) if ui else add('ui.json', 'ui.json')
    add(f"icon-{target['icon']}.bin", 'icon.bin')
    for source in sorted(p for p in paths if p.startswith('files/')):
        path, _ = data_file_path(source, manifest)
        if path:
            add(source, path)
    if isinstance(manifest.get('elf'), str) and manifest.get('elf') and \
            not add(f"elf/{target['arch']}/app.elf", 'app.elf'):
        return out, f"缺 {target['arch']} ELF"
    return out, None


def icon_problem(data, size):
    if len(data) < 12 or data[:8] != b'BOBOICN1':
        return '不是 BOBOICN1 位图（用打包工具从 icon.png 生成）'
    w, h = data[8] | data[9] << 8, data[10] | data[11] << 8
    if w != size or h != size:
        return f'尺寸是 {w}×{h}，应为 {size}×{size}'
    if len(data) != 12 + h * ((w + 7) // 8):
        return f'长度 {len(data)} 字节与 {w}×{h} 不符'
    if len(data) > LIMITS['icon']:
        return '超过 1 KB'
    if size not in (40, 56):
        return '只有 icon-40.bin（3.97 寸）和 icon-56.bin（4.7 寸）会被使用'
    return ''


def elf_code(message):
    """elfcheck's messages -> the store's issue codes."""
    rules = [('not an ELF', 'elf_not_elf'), ('ELF class', 'elf_not_elf'), ('e_machine', 'elf_machine'),
             ('built for', 'elf_machine'), ('float ABI', 'elf_float_abi'), ('e_type', 'elf_type'),
             ('bytes: app.elf may be at most', 'elf_too_large'), ('no .text', 'elf_no_text'),
             ('relocation', 'elf_reloc'), ('does not export', 'elf_unknown_symbol'), ('outside the file', 'elf_corrupt')]
    for needle, code in rules:
        if needle in message:
            return code
    return 'elf_corrupt'


def inspect(files, package_size, host=None):
    """The report for a package's files ({path: bytes}, root folder already stripped)."""
    r = Report()
    host_api, host_syms = host or elfcheck.load_host_symbols()
    result = {'ok': False, 'host_api': host_api, 'package_size': package_size, 'app': None, 'boards': [],
              'issues': r.issues}
    bad = [p for p in files if not SAFE_PATH.match(p) or '..' in p]
    if bad:
        r.add('error', 'bad_path', f"不支持的文件路径：{'、'.join(bad[:5])}（仅限字母数字 . _ -，最多 4 层）")
    if package_size > LIMITS['package']:
        r.add('error', 'package_too_large', f'应用包有 {package_size / 1048576:.2f} MB，上限 2 MB')
    app_data = files.get('app.json')
    if app_data is None:
        r.add('error', 'missing_app_json', '包里没有 app.json（原生应用的清单，放在 zip 根目录）')
        return finish(r, result)
    manifest = parse_json(app_data) if len(app_data) <= LIMITS['manifest'] else None
    if manifest is None:
        r.add('error', 'app_json_invalid', 'app.json 不是合法的 JSON 对象，或超过 8 KB', 'app.json')
        return finish(r, result)

    app_id = str(manifest.get('id') or '').strip()
    version = str(manifest.get('version') or '').strip()
    api = manifest.get('api', 1)
    if not APP_ID.match(app_id):
        r.add('error', 'bad_id', f'app.json 的 id「{app_id}」只能包含小写字母、数字和连字符（3–40 位）', 'app.json')
    elif app_id in RESERVED:
        r.add('error', 'reserved_id', f'应用 ID「{app_id}」是保留名称（系统内置应用或网站路由）', 'app.json')
    if not SEMVER.match(version):
        r.add('error', 'bad_version', 'app.json 的 version 必须是 "X.Y.Z" 字符串', 'app.json')
    api_ok = isinstance(api, int) and not isinstance(api, bool) and api >= 1
    if not api_ok:
        r.add('error', 'bad_api', 'app.json 的 api 必须是正整数', 'app.json')
    elif api > host_api:
        r.add('error', 'api_too_new', f'app.json 声明 api {api}，这份 SDK 的固件最高支持 API {host_api}', 'app.json')
    if not str(manifest.get('name') or '').strip():
        r.add('warning', 'name_missing', 'app.json 没有 name，启动器会显示应用 ID', 'app.json')
    declared_api = api if api_ok else 1
    cloud_only = manifest.get('cloud_only') is True
    wants_elf = isinstance(manifest.get('elf'), str) and bool(manifest.get('elf'))

    raw_ui = manifest.get('ui') if isinstance(manifest.get('ui'), str) else ''
    ui = ui_file_name(manifest)
    if raw_ui.strip() and not ui:
        r.add('error', 'ui_bad_name', f'app.json 的 "ui" 必须是包根目录里的文件名，不能是「{raw_ui}」', 'app.json')
    if ui:
        data = files.get(ui)
        if data is None:
            r.add('error', 'ui_missing', f'app.json 写了 "ui": "{ui}"，但包里没有这个文件', ui)
        else:
            parsed = parse_json(data) if len(data) <= LIMITS['ui'] else None
            if parsed is None or parsed.get('schema', 1) != 1:
                r.add('error', 'ui_invalid', f'{ui} 不是合法的 JSON 对象、超过 64 KB 或 schema 不是 1', ui)
    elif not raw_ui.strip() and 'ui.json' in files:
        r.add('warning', 'ui_undeclared', '包里有 ui.json，但 app.json 没写 "ui": "ui.json"，设备不会使用它', 'ui.json')
    mcp = files.get('mcp.json')
    if mcp is not None:
        if (parse_json(mcp) if len(mcp) <= LIMITS['mcp'] else None) is None:
            r.add('error', 'mcp_invalid', 'mcp.json 不是合法的 JSON 对象或超过 32 KB', 'mcp.json')
        if not str(manifest.get('mcp') or '').strip():
            r.add('warning', 'mcp_undeclared', '包里有 mcp.json，但 app.json 没写 "mcp": "mcp.json"，AI 工具不会启用', 'mcp.json')
    elif str(manifest.get('mcp') or '').strip():
        r.add('error', 'mcp_missing', 'app.json 写了 "mcp"，但包里没有 mcp.json', 'app.json')
    if cloud_only:
        r.add('info', 'cloud_only', '这个应用只在 BoBo 云端运行（cloud_only），设备商店不列出它', 'app.json')

    elf_files = [p for p in files if p.startswith('elf/')]
    unknown = [p for p in elf_files if not ELF_ENTRY.match(p)]
    if unknown:
        r.add('error', 'elf_unknown_target', f"elf/ 下只能有 elf/s3、elf/s31 与 elf/sf32lb57 的 app.elf，多出：{'、'.join(unknown)}")
    elf_ok, api_used = {}, declared_api
    for path in sorted(p for p in elf_files if ELF_ENTRY.match(p)):
        target = path.split('/')[1]
        rep = elfcheck.check_bytes(files[path], path, target, None, (host_api, host_syms))
        for w in rep['warnings']:
            r.add('warning', 'elf_reloc_none' if 'R_XTENSA_NONE' in w else 'elf_section', w, path)
        for e in rep['errors']:
            if 'newer than this SDK' in e:
                continue
            r.add('error', elf_code(e), e, path)
        ok = rep['ok'] or all('newer than this SDK' in e for e in rep['errors'])
        if api_ok and rep['required_api'] > declared_api:
            names = sorted(i['name'] for i in rep['imports'] if i['since'] > declared_api)
            r.add('error', 'api_mismatch', f"{path} 用到了 API {rep['required_api']} 的函数 {'、'.join(names[:8])}，"
                  f"但 app.json 只声明了 api {declared_api}；请改成 \"api\": {rep['required_api']}", path)
            ok = False
        api_used = max(api_used, rep['required_api'])
        elf_ok[target] = ok
    if wants_elf and not elf_ok and not unknown:
        r.add('error', 'elf_missing', 'app.json 声明了 elf，但包里没有 elf/s3、elf/s31 或 elf/sf32lb57 的 app.elf')
    if not wants_elf and elf_ok:
        r.add('warning', 'elf_undeclared', '包里有 ELF，但 app.json 没写 "elf": "app.elf"，设备不会加载它', 'app.json')

    for path in [p for p in files if ICON.match(p)]:
        problem = icon_problem(files[path], int(ICON.match(path).group(1)))
        if problem:
            r.add('error', 'icon_invalid', f'{path}：{problem}', path)

    data = [p for p in files if p.startswith('files/')]
    if len(data) > LIMITS['data_files']:
        r.add('error', 'files_too_many', f"files/ 下有 {len(data)} 个文件，上限 {LIMITS['data_files']}")
    for p in data:
        _, reason = data_file_path(p, manifest)
        if reason == 'path':
            r.add('error', 'files_bad_path', f'{p}：设备路径只能有一层子目录、不超过 64 个字符，且每段以字母或数字开头', p)
        elif reason == 'reserved':
            r.add('error', 'files_reserved', f'{p} 会覆盖应用目录里的系统文件', p)

    blocking = [i for i in r.errors() if not str(i.get('file', '')).startswith('elf/') and i['code'] != 'icon_invalid']
    paths = set(files)
    for board, b in BOARDS.items():
        delivered, missing = board_delivery(manifest, paths, board)
        size = sum(len(files[s]) for s, _ in delivered)
        icon_bad = any(i['code'] == 'icon_invalid' and i.get('file') == f"icon-{b['icon']}.bin" for i in r.issues)
        reason = ''
        if cloud_only:
            reason = '只在云端运行'
        elif blocking:
            reason = f"包有错误（{blocking[0]['code']}）"
        elif missing:
            reason = missing
        elif wants_elf and b['arch'] in elf_ok and not elf_ok[b['arch']]:
            reason = f"{b['arch']} ELF 有错误"
        elif icon_bad:
            reason = f"icon-{b['icon']}.bin 格式不对"
        elif size > LIMITS['install']:
            reason = f'要下载 {size / 1048576:.2f} MB，超过 2 MB'
            r.add('error', 'install_too_large', f"{b['label']}：{reason}（设备商店会忽略这个应用）")
        result['boards'].append({'board': board, 'label': b['label'], 'arch': b['arch'], 'installable': not reason,
                                 'reason': reason, 'size': size, 'files': [s for s, _ in delivered]})
    if not cloud_only and not r.errors() and not any(v['installable'] for v in result['boards']):
        r.add('error', 'no_board', '没有任何主板能装这个包')
    iconless = [v for v in result['boards'] if v['installable'] and f"icon-{BOARDS[v['board']]['icon']}.bin" not in paths]
    if iconless:
        r.add('warning', 'icon_missing', '、'.join(f"{v['label']}（icon-{BOARDS[v['board']]['icon']}.bin）" for v in iconless)
              + ' 没有启动器图标')
    delivered = {s for v in result['boards'] for s in v['files']}
    ignored = sorted(p for p in paths if p not in delivered and not ELF_ENTRY.match(p) and not ICON.match(p)
                     and not p.startswith('files/'))
    if ignored:
        r.add('info', 'ignored_files', f"不会安装到设备：{'、'.join(ignored[:8])}")
    result['app'] = {'id': app_id, 'version': version, 'name': str(manifest.get('name') or ''), 'api': declared_api,
                     'api_used': api_used, 'elf': wants_elf, 'ui': ui, 'mcp': mcp is not None, 'cloud_only': cloud_only}
    return finish(r, result)


def finish(r, result):
    order = {'error': 0, 'warning': 1, 'info': 2}
    r.issues.sort(key=lambda i: order[i['level']])
    result['ok'] = not r.errors()
    if not result['boards']:
        first = r.errors()[0]['code'] if r.errors() else ''
        result['boards'] = [{'board': k, 'label': b['label'], 'arch': b['arch'], 'installable': False,
                             'reason': f'包有错误（{first}）', 'size': 0, 'files': []} for k, b in BOARDS.items()]
    return result


def check_zip(path):
    data = Path(path).read_bytes()
    try:
        files = read_zip(data)
    except ValueError as e:
        r = Report()
        r.add('error', 'zip_invalid', f'{e}（请用 zip 格式打包，不要加密）')
        return finish(r, {'ok': False, 'app': None, 'boards': [], 'issues': r.issues, 'package_size': len(data)})
    return inspect(files, len(data))


def print_report(report, file=None):
    app = report.get('app') or {}
    if app:
        uses = f"ELF needs {app.get('api_used')}" if app.get('elf') else 'no ELF'
        print(f"{app.get('id')} {app.get('version')}  api {app.get('api')} ({uses})")
    for b in report['boards']:
        state = f"OK, {b['size'] / 1024:.0f} KB" if b['installable'] else f"NO: {b['reason']}"
        print(f"  {b['label']:12} ({b['board']}): {state}")
    for i in report['issues']:
        print(f"  {i['level'].upper():7} {i['code']}: {i['message']}")
    print('OK' if report['ok'] else 'REJECTED (the store would refuse this upload)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('package')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--board', choices=list(BOARDS))
    args = ap.parse_args()
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')  # GBK consoles
    report = check_zip(args.package)
    ok = report['ok']
    if args.board:
        ok = ok and next(b for b in report['boards'] if b['board'] == args.board)['installable']
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=1))
    else:
        print_report(report)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
