# SDK 思澈目标覆盖文件

这些文件基于 `bobo-app-sdk-2026.11.19`，只增加了 4.7 寸思澈（`sf57` / `sf32lb57`）构建目标，不改动 S3、S31 的行为。

覆盖：

```bash
cp build.py                 "$BOBO_SDK/build.py"
cp pack.py                  "$BOBO_SDK/pack.py"
cp tools/elfcheck.py        "$BOBO_SDK/tools/elfcheck.py"
cp tools/pkgcheck.py        "$BOBO_SDK/tools/pkgcheck.py"
```

改动点：

- `build.py`：新增 `sf57` 目标，ARM 硬浮点、`-mlong-calls`、`-fno-plt`；安装目录名为 `sf32lb57`
- `pack.py`：默认目标改为 `s3,s31,sf57`，按 `arch` 写入 `elf/sf32lb57/app.elf`
- `tools/elfcheck.py`：接受 `EM_ARM`，只允许思澈加载器支持的重定位
- `tools/pkgcheck.py`：登记 `sifli_gyd7n6` 主板，接受 `elf/sf32lb57/app.elf`

如果官方 SDK 后续自带思澈目标，应优先使用官方版本，这些文件仅作为参考。
