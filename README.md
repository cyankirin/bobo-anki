# Anki 卡片（bobo-anki）

在 BoBo 墨水屏阅读器上读 TF 卡里的 Anki 卡组（`.apkg`），按 Anki 的间隔重复逻辑复习，学习进度保存在设备上。

纯 C 实现，不依赖第三方库：自带 DEFLATE 解压、ZIP 读取、SQLite 只读解析和 Anki 调度器。

## 支持机型

同一个安装包里带三个 ELF，设备按主板自动选择：

| 机型 | 主板 | 目标 | 架构 |
| --- | --- | --- | --- |
| 3.97 寸 | `metalio_eink4` | `s3` | Xtensa（ESP32-S3） |
| 4.7 寸 Plus | `metalio_eink4plus` | `s31` | RISC-V（ESP32-S31，ilp32f） |
| 4.7 寸 思澈 | `sifli_gyd7n6` | `sf57` | ARMv8-M（SF32LB57，EABI5 硬浮点） |

思澈版目前完成编译和 ELF 结构校验，尚未在真机验收。

## 安装

1. 把 `bobo-anki-<版本>.zip` 复制到 TF 卡的 `/apps-import/`。
2. 在设备上打开「应用商店 › 导入」，选择该 zip。
3. 也可以把解压后的应用目录放进 TF 卡的 apps 目录，或用开发工具 `bobo push bobo-anki --run`。

## 使用

1. 在 TF 卡根目录建 `Anki` 文件夹，把 `.apkg` 卡组复制进去，可以放多个。
2. 打开应用：选卡组文件 → 选牌组 → 学习。
3. 普通卡点屏幕翻答案；逐句挖空卡按原文宽度生成下划线，点空白行逐行揭示，也可点右下角「显示全部」。
4. 四档评分：忘记 / 困难 / 良好 / 简单。侧键上/下也能显示答案与评为「良好」，返回键退到上一级。
5. 学习页左上角「撤销上一步」是独立大按钮，可退回刚评过的那张卡。
6. 文件列表底部提示「长按 apkg 可查看学习统计」：长按某个 apkg 会打开操作页，可查看该文件的学习统计、备份进度、恢复进度，也可恢复本机隐藏的牌组。
7. 牌组列表长按 0 张卡的空牌组（常见是 Default）可删除，删除状态保存在本机应用目录，重新打开同一个 apkg 仍然生效。

## 卡组兼容

### 题型识别

按笔记模板的字段名判断：

- `Q` / 含「问题」「题干」「question」「front」→ 题干
- `T` / 含「答案」「answer」「correct」→ 正确答案
- `A`–`D` / 含「选项」「option」→ 选项
- 含「背面」「back」「释义」→ 背面
- 有题干和背面、选项少于两个 → 普通正反面卡（basic）
- 识别不出时退回「第一个字段正面、其余字段背面」

选择题会统一加 `A.` `B.` `C.` `D.` 前缀；点开答案后只保留正确项，错误项直接隐去，并在上方标出正确字母。

### 逐句挖空模板

同时含 `span class="d"`（挖空）和 `span class="t"`（原文）的模板（如「古诗文·逐句回想」）：

- 正面按每行 `width:NNem` 生成全角下划线，逐行显示
- 点哪一行就翻开哪一行的原文
- 全部翻开后自动进入评分页；也可点「显示全部」一次翻开

### 文件格式

- 优先读 `collection.anki21`，没有时读 `collection.anki2`
- `collection.anki21b`（Anki 2.1.50+ 默认导出的 zstd 格式）不支持，需要重新导出时勾选「支持旧版 Anki」
- 超过 4 MB 的 apkg 只读前 4 MB 前缀，用来取到卡组库；图片和音频不显示

## 学习逻辑

- 新卡 → 学习步进（默认 1 分钟、10 分钟）→ 毕业进入复习（间隔 1 天）；评「简单」直接给 4 天
- 复习卡按 ease（初始 250%）乘算：忘记 → 重学并降 ease、记一次 lapse；困难 ×1.2；良好 ×ease；简单 ×1.3 并升 ease
- 每日上限读 `col.dconf` 的 `new.perDay` / `rev.perDay`（默认 20 / 200），按牌组分别计数
- 某个牌组学满后，牌组列表出现「继续学习 20 张」
- 统计页显示各牌组剩余量和今日答题数、「记得」百分比

## 进度与备份

- 每个 apkg 单独一个进度文件：`user/s<8 位路径哈希>.bin`，备份为 `user/b<8 位路径哈希>.bin`
- 删除的空牌组记在 `user/h<8 位路径哈希>.bin`，只影响本机显示；TF 卡对应用只读，不会改写原 apkg
- 切换卡组不会互相覆盖；打开新卡组前会先保存当前卡组
- 0.2.3 及更早版本共用 `user/srs.bin`，首次加载会自动迁移一次
- TF 卡对应用只读，备份只能留在应用目录，不能直接导出到卡上

## 刷新策略

- 启动首屏、系统 `BOBO_EV_REDRAW` 用 `HALF`
- 翻页、逐行揭示、看答案、评分、滑动等交互全部用 `FAST`
- 不使用 `FULL`，残影交给系统定期清理
- 不再每次点击都整屏 HALF，长诗逐行点开时耗电明显下降

## 构建

需要 BoBo 应用 SDK `bobo-app-sdk-2026.11.19`（API 6）和三个工具链：

- `xtensa-esp-elf-gcc`（S3）
- `riscv32-esp-elf-gcc`（S31）
- `arm-none-eabi-gcc` 13.3（思澈 SF32LB57）

官方 SDK 只有 `s3` / `s31` 两个目标。思澈目标 `sf57` 的改动在 `docs/sdk/overlay/`，覆盖到 SDK 对应文件即可：

```bash
cp -r docs/sdk/overlay/* "$BOBO_SDK/"
```

然后编译和打包：

```bash
export PATH="$ARM_TOOLCHAIN/bin:$RISCV_TOOLCHAIN/bin:$XTENSA_TOOLCHAIN/bin:$PATH"
python3 "$BOBO_SDK/build.py" bobo-anki --all --out bobo-anki/dist
python3 "$BOBO_SDK/pack.py" bobo-anki --targets s3,s31,sf57 --out bobo-anki/dist
```

产物是 `dist/bobo-anki-<版本>.zip`，包含：

```text
app.json
elf/s3/app.elf
elf/s31/app.elf
elf/sf32lb57/app.elf
icon-40.bin
icon-56.bin
```

细节见 [docs/BUILD.md](docs/BUILD.md)。思澈构建必须硬浮点、`-mlong-calls`、`-fno-plt`，不能出现 `.plt` 或 `R_ARM_JUMP_SLOT`。

## 项目结构

```text
src/inflate.c   DEFLATE 解压
src/zip.c       ZIP 读取（store / deflate、EOCD、中央目录、local header）
src/sqlite.c    SQLite 只读 B 树解析
src/anki.c      apkg 解析、题型识别、Anki 调度、进度读写
src/main.c      文件/牌组/学习/统计页与触摸、按键处理
docs/           开发文档与 SDK 思澈目标覆盖文件
```

## 已知限制

- 不执行 Anki 模板的 JavaScript 和 CSS，不处理 `<input>`、图片、音频
- 不联网、不同步，统计只汇总当天
- 单日进度按设备本地时区分界
- 卡数上限 12000，牌组上限 64；单字段清洗后上限 1536 字节，单卡正/背面上限 3072 字节（约 1000 个汉字）
- 删除空牌组只在本机隐藏，不能写回 TF 卡上的 apkg（SDK 的 `bobo_sd_*` 只有只读接口）
- 学习步进与每日上限读卡组配置；自定义步骤差异很大时按 1 分钟 / 10 分钟近似

## 许可

MIT，见 [LICENSE](LICENSE)。
