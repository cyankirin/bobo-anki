"""Validate a BoBo app.elf the way the device loader will treat it (pure Python, no toolchain).

usage: python app-sdk/tools/elfcheck.py <app.elf> [--target s3|s31] [--api N] [--json]

Checks, in the order the device fails:
  1. ELF header: 32-bit little-endian shared object for the board's CPU (s3: Xtensa,
     s31: RISC-V with the ilp32f float ABI). The firmware refuses anything else with
     "此应用不支持本设备" before relocating.
  2. Relocations: only the types lib/elf_loader/src/arch/esp_elf_{xtensa,riscv}.c apply.
     Anything else (R_RISCV_HI20 from non-PIC code, ...) fails the load.
  3. Imports: every undefined symbol must be in app-sdk/host_symbols.json (the firmware's
     export table); the highest API level among them must not exceed app.json "api".
  4. Size: app.elf at most 512 KB (ElfAppActivity kMaxElfBytes).
Exit code 0 when the ELF is accepted. Used by build.py after linking and by bobo.py check;
a server can run the same file (it needs only the Python standard library).
"""
import argparse
import json
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SDK = HERE.parent
MAX_ELF_BYTES = 512 * 1024
MACHINES = {40: 'sf57', 94: 's3', 243: 's31'}  # EM_ARM, EM_XTENSA, EM_RISCV
MACHINE_NAMES = {40: 'ARMv8-M (SF32LB57)', 94: 'Xtensa (ESP32-S3)', 243: 'RISC-V (ESP32-S31)'}
# The relocation types the loader applies (lib/elf_loader/src/arch/esp_elf_*.c).
RELOC_OK = {
    's3': {2: 'R_XTENSA_RTLD', 3: 'R_XTENSA_GLOB_DAT', 4: 'R_XTENSA_JMP_SLOT', 5: 'R_XTENSA_RELATIVE'},
    's31': {0: 'R_RISCV_NONE', 1: 'R_RISCV_32', 3: 'R_RISCV_RELATIVE', 5: 'R_RISCV_JUMP_SLOT'},
    'sf57': {0: 'R_ARM_NONE', 2: 'R_ARM_ABS32', 3: 'R_ARM_REL32', 21: 'R_ARM_GLOB_DAT', 23: 'R_ARM_RELATIVE'},
}
RELOC_NAMES = {
    's3': {0: 'R_XTENSA_NONE', 1: 'R_XTENSA_32', 6: 'R_XTENSA_PLT', 20: 'R_XTENSA_SLOT0_OP'},
    's31': {2: 'R_RISCV_64', 4: 'R_RISCV_COPY', 18: 'R_RISCV_CALL', 19: 'R_RISCV_CALL_PLT', 20: 'R_RISCV_GOT_HI20',
            23: 'R_RISCV_PCREL_HI20', 24: 'R_RISCV_PCREL_LO12_I', 26: 'R_RISCV_HI20', 27: 'R_RISCV_LO12_I',
            28: 'R_RISCV_LO12_S'},
    'sf57': {2: 'R_ARM_ABS32', 17: 'R_ARM_TLS_DTPMOD32', 18: 'R_ARM_TLS_DTPOFF32', 19: 'R_ARM_TLS_TPOFF32'},
}
SHT_RELA, SHT_DYNSYM, SHT_NOBITS = 4, 11, 8


def load_host_symbols(path=None):
    data = json.loads(Path(path or SDK / 'host_symbols.json').read_text(encoding='utf-8'))
    return data['api'], {s['name']: s for s in data['symbols']}


class Elf:
    def __init__(self, data):
        self.data = data
        if len(data) < 52 or data[:4] != b'\x7fELF':
            raise ValueError('not an ELF file')
        self.ei_class, self.ei_data = data[4], data[5]
        if self.ei_class != 1 or self.ei_data != 1:
            raise ValueError(f'ELF class {self.ei_class} data {self.ei_data}: need 32-bit little-endian')
        (self.type, self.machine, _, self.entry, self.phoff, self.shoff, self.flags, _, _, self.phnum, self.shentsize,
         self.shnum, self.shstrndx) = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
        if self.shentsize != 40 or self.shoff + self.shnum * 40 > len(data) or self.shstrndx >= self.shnum:
            raise ValueError('section headers outside the file')
        self.sections = []
        for i in range(self.shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
                '<IIIIIIIIII', data, self.shoff + 40 * i)
            if typ != SHT_NOBITS and off + size > len(data):
                raise ValueError(f'section {i} outside the file')
            self.sections.append(dict(name_off=name, type=typ, flags=flags, addr=addr, off=off, size=size, link=link,
                                      info=info, entsize=entsize))
        strtab = self.sections[self.shstrndx]
        for s in self.sections:
            s['name'] = self._str(strtab, s['name_off'])

    def _str(self, table, off):
        start = table['off'] + off
        end = self.data.index(b'\0', start)
        return self.data[start:end].decode('utf-8', 'replace')

    def symbols(self, index):
        sec = self.sections[index]
        strtab = self.sections[sec['link']]
        out = []
        for i in range(sec['size'] // 16):
            name, value, size, info, other, shndx = struct.unpack_from('<IIIBBH', self.data, sec['off'] + 16 * i)
            out.append(dict(name=self._str(strtab, name), value=value, size=size, bind=info >> 4, kind=info & 15,
                            shndx=shndx))
        return out

    def undefined(self):
        names = set()
        for i, s in enumerate(self.sections):
            if s['type'] == SHT_DYNSYM:
                names |= {sym['name'] for sym in self.symbols(i) if sym['shndx'] == 0 and sym['name']}
        return sorted(names)

    def relocations(self):
        """[(section name, type, symbol name)]"""
        out = []
        for s in self.sections:
            if s['type'] != SHT_RELA:
                continue
            syms = self.symbols(s['link']) if s['link'] and self.sections[s['link']]['type'] == SHT_DYNSYM else []
            for i in range(s['size'] // 12):
                _, info, _ = struct.unpack_from('<IIi', self.data, s['off'] + 12 * i)
                sym = info >> 8
                out.append((s['name'], info & 0xFF, syms[sym]['name'] if sym < len(syms) else ''))
        return out


def check(path, target=None, api=None, host=None):
    """Returns a report dict: ok, errors [str], warnings [str], target, required_api, imports, size."""
    return check_bytes(Path(path).read_bytes(), str(path), target, api, host)


def check_bytes(data, name, target=None, api=None, host=None):
    """check() for an image in memory (pkgcheck.py reads it from the zip)."""
    report = {'file': name, 'size': len(data), 'errors': [], 'warnings': [], 'imports': [], 'required_api': 1}
    errors = report['errors']
    try:
        elf = Elf(data)
    except ValueError as e:
        errors.append(f'{e} (rebuild with app-sdk/build.py)')
        report['ok'] = False
        return report
    found = MACHINES.get(elf.machine)
    report['target'] = found
    if not found:
        errors.append(f'e_machine {elf.machine} is not an ESP32 app')
    elif target and found != target and not (target in ('sf32lb57', 'sf57') and found == 'sf57'):
        errors.append(f'built for {MACHINE_NAMES[elf.machine]} but checked as --target {target}: '
                      f'the device shows "此应用不支持本设备"; build with --target {target}')
    if found == 's31' and (elf.flags & 0x6) != 0x2:
        errors.append(f'float ABI flags 0x{elf.flags:x}: the firmware passes floats in FP registers (ilp32f); '
                      'build with app-sdk/build.py --target s31')
    if elf.type != 3:
        errors.append(f'e_type {elf.type}: need a shared object (ET_DYN); link with -shared (build.py does)')
    if len(data) > MAX_ELF_BYTES:
        errors.append(f'{len(data)} bytes: app.elf may be at most {MAX_ELF_BYTES} (move data to files/)')
    names = {s['name'] for s in elf.sections}
    if '.text' not in names:
        errors.append('no .text section')
    if found:
        bad = {}
        for sec, typ, sym in elf.relocations():
            if found == 's3' and typ == 0:
                # R_XTENSA_NONE: the loader logs "not supported" and skips it (the store warns, elf_reloc_none).
                if 'R_XTENSA_NONE relocation (harmless)' not in report['warnings']:
                    report['warnings'].append('R_XTENSA_NONE relocation (harmless)')
                continue
            if typ not in RELOC_OK[found]:
                name = RELOC_NAMES[found].get(typ, f'type {typ}')
                bad.setdefault(name, set()).add(sym or sec)
        for name, where in sorted(bad.items()):
            errors.append(f'relocation {name} ({", ".join(sorted(where)[:6])}): the loader cannot apply it. '
                          'Non-PIC code was linked in (often libgcc or a prebuilt .a): see docs/errors.md')
    host_api, host_syms = host if host else load_host_symbols()
    required = 1
    for name in elf.undefined():
        sym = host_syms.get(name)
        if not sym:
            errors.append(f'imports {name}, which the firmware does not export (host_symbols.json): '
                          'the load fails with "ELF needs unknown symbol"')
            continue
        report['imports'].append({'name': name, 'since': sym['since']})
        required = max(required, sym['since'])
    report['required_api'] = required
    if required > host_api:
        errors.append(f'needs API {required}, newer than this SDK ({host_api})')
    if api is not None and required > api:
        needing = sorted(i['name'] for i in report['imports'] if i['since'] > api)
        errors.append(f'app.json says "api": {api} but {", ".join(needing[:8])} need API {required}: '
                      f'set "api": {required}')
    report['ok'] = not errors
    return report


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('elf')
    ap.add_argument('--target', choices=('s3', 's31', 'sf57'))
    ap.add_argument('--api', type=int)
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args()
    report = check(args.elf, args.target, args.api)
    if args.json:
        print(json.dumps(report, ensure_ascii=False, indent=1))
    else:
        print(f"{report['file']}: {report['size']} bytes, target {report.get('target')}, "
              f"needs API {report['required_api']}, {len(report['imports'])} imports")
        for e in report['errors']:
            print('ERROR', e)
        print('OK' if report['ok'] else 'REJECTED')
    sys.exit(0 if report['ok'] else 1)


if __name__ == '__main__':
    main()
