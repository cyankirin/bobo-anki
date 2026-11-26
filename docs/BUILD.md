# 构建与打包

## 依赖

- BoBo 应用 SDK `bobo-app-sdk-2026.11.19`（API 6）
- Arm GNU Toolchain 13.3（`arm-none-eabi-`，思澈）
- PlatformIO 的 `toolchain-riscv32-esp`（S31）
- PlatformIO 的 `toolchain-xtensa-esp-elf`（S3）
- Python 3

## SDK 思澈目标

官方 SDK 的 `build.py` 只有 `s3` / `s31`。思澈机型（`sifli_gyd7n6`，主板 SF32LB57）要求：

- ARMv8-M / Cortex-M33，Thumb
- EABI5 硬浮点，`e_flags = 0x5000400`
- 函数调用用 `-mlong-calls`
- 不能有 `.plt`
- 只允许 `R_ARM_NONE / ABS32 / REL32 / GLOB_DAT / RELATIVE`

仓库里的 `docs/sdk/overlay/` 是改好的 SDK 文件，直接覆盖：

```bash
cp docs/sdk/overlay/build.py        "$BOBO_SDK/build.py"
cp docs/sdk/overlay/pack.py         "$BOBO_SDK/pack.py"
cp docs/sdk/overlay/tools/elfcheck.py "$BOBO_SDK/tools/elfcheck.py"
cp docs/sdk/overlay/tools/pkgcheck.py "$BOBO_SDK/tools/pkgcheck.py"
```

覆盖后：

- `build.py --target sf57`：硬浮点 ARM 构建
- `pack.py --targets s3,s31,sf57`：三个 ELF 打进同一个包
- 思澈 ELF 在包里的目录是 `elf/sf32lb57/app.elf`（商店要求）
- `pkgcheck.py` 认识 `sifli_gyd7n6`
- `elfcheck.py` 接受 ARM 机器类型和思澈允许的重定位

### sf57 关键编译参数

```text
-mcpu=cortex-m33 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16
-mlong-calls -fno-plt -mno-unaligned-access
```

链接仍然使用 SDK 的：

```text
-nostartfiles -nostdlib -shared -e app_main -Wl,--gc-sections -Wl,-z,defs
```

`-fno-plt` 避免外部函数调用生成 `.plt` 和 `R_ARM_JUMP_SLOT`；`-mlong-calls` 让调用走字面池里的地址，产生加载器能处理的 `GLOB_DAT` / `RELATIVE`。

## 编译

```bash
export PATH="$ARM_TOOLCHAIN/bin:$RISCV_TOOLCHAIN/bin:$XTENSA_TOOLCHAIN/bin:$PATH"

python3 "$BOBO_SDK/build.py" bobo-anki --all --out bobo-anki/dist
# 或者只编一个
python3 "$BOBO_SDK/build.py" bobo-anki --target sf57 --out bobo-anki/dist
```

## 打包

```bash
python3 "$BOBO_SDK/pack.py" bobo-anki --targets s3,s31,sf57 --out bobo-anki/dist
```

包结构：

```text
app.json
elf/s3/app.elf             Xtensa
elf/s31/app.elf            RISC-V ilp32f
elf/sf32lb57/app.elf       ARM EABI5 hard-float
icon-40.bin                3.97 寸
icon-56.bin                4.7 寸 Plus / 思澈
```

## 上架校验常见错误

| 错误 | 原因 | 处理 |
| --- | --- | --- |
| `elf_unknown_target` | 思澈 ELF 放在 `elf/sifli/` | 必须放 `elf/sf32lb57/app.elf` |
| `elf_machine`（e_flags `0x5000200`） | 软浮点 | 改 `-mfloat-abi=hard -mfpu=fpv5-sp-d16` |
| `elf_reloc`（有 `.plt`） | 函数调用走 PLT | 加 `-mlong-calls -fno-plt` |
| `elf_reloc`（`R_ARM_JUMP_SLOT` ×N） | 同上 | 同上，确认 ELF 里没有 `.plt` |
| 导入未知符号 | 调了固件没导出的函数 | 只用 SDK `include/bobo_app.h` 与 libc 子集 |

本地自检：

```bash
python3 "$BOBO_SDK/tools/elfcheck.py" dist/sf57/bobo-anki/app.elf --target sf57 --api 6
arm-none-eabi-readelf -h  dist/sf57/bobo-anki/app.elf   # Flags: 0x5000400 hard-float
arm-none-eabi-readelf -S  dist/sf57/bobo-anki/app.elf   # 不应有 .plt
arm-none-eabi-readelf -r  dist/sf57/bobo-anki/app.elf   # 只有 GLOB_DAT / RELATIVE
```

## 设备限制

- ELF 最大 512 KB
- 单个安装包最大 2 MB
- 数据文件最多 16 个，目录最多一层
- 应用只能读写自己的 `user/` 目录，TF 卡上的 apkg 只读
