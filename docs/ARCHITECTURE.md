# 实现说明

## 总体结构

```text
main.c     界面状态机、绘制、触摸/按键
anki.c     apkg 数据模型、模板识别、调度、进度
zip.c      ZIP 容器
inflate.c  DEFLATE 解压
sqlite.c   SQLite 只读
```

全部是 C11，不引入第三方库，也不使用浮点（固件不导出 `atof`，早期用浮点解析挖空宽度时链接失败）。

## apkg 读取链路

1. `bobo_sd_*` 打开 TF 卡文件；超过 4 MB 的包只读前 4 MB。
2. `zip_open` 读 EOCD，失败时允许无 EOCD（大包前缀场景）。
3. `zip_find` 先查中央目录，找不到时从文件头顺序扫描 local header（`find_local`），定位 `collection.anki21` / `collection.anki2`。
4. `inflate` 解压，上限 4 MB。
5. `sqlite_open` 打开解压后的库，只读 B 树。

### SQLite 解析

- 页大小、表根页从 `sqlite_master` 取
- 支持 `int` / `text` / `blob` / `null` 的 serial type
- varint 解码
- 溢出页拼接
- 只做 `SELECT` 语义的遍历，不写库

### Anki 数据

- `col.conf`：解析 `new` / `rev` / `decks`（早期版本该字段可能缺失，所有 JSON 取值都判空）
- `col.decks`：牌组 id 为 64 位
- `col.dconf`：每日上限，默认 20 / 200
- `notes`：字段用 `\x1f` 分隔
- `cards`：`did`、`nid`、`ord`、`type`、`queue`、`due`、`ivl`、`factor`、`reps`、`lapses`

支持 `collection.anki21` 为空壳兼容库、真实数据在 `collection.anki21`（0.1.1 修复的 2025.apkg 就是这种结构）。

## 模板与题型

字段名规则：

- `Q` / 问题 / 题干 / question / front → 题干
- `T` / 答案 / answer / correct → 答案
- `A`–`D` / 选项 / option → 选项
- 背面 / back / 释义 → 背面
- 有题干 + 背面且选项 < 2 → `basic`

渲染分三条路径：

1. 选项模式：题号、题干、A–D 选项；答案页只留正确项
2. basic 模式：题干前的字段作标题（Title/Author），正面题干，背面正文
3. 兜底：首字段正面，其余字段背面

### 逐句挖空

`build_blanks` 扫描 `span class="d"` 和 `span class="t"`：

- `width:NNem` 解析成整数（不能用 `atof`）
- 每行生成 `card_line_t { blanks, text }`
- 正面画 `＿`，点开后画原文
- `nlines` 上限 40，`g_reveal` 位掩码记录已揭示的行
- 全部揭示后 `g_show_back = 1` 进评分页；「显示全部」直接置满

早期坑：取 `<span class="t">` 文本时偏移少 1，导致每行前面多一个 `>`；改为先 `strchr('>') + 1` 再截到 `</span>`。

## 调度

- `new`：学习步进 1 分钟、10 分钟；毕业间隔 1 天；评「简单」4 天
- `lrn`：按步进推进
- `rev`：`ivl * factor`，四档系数 0 / 1.2 / ease / 1.3
- 忘记：进重学，`lapses++`，ease 降 2000（最低 1300）
- 每日配额按牌组独立计数，`new_done` / `rev_done` 与日期一起存进度

## 进度文件

`progress_name()` 用 FNV-1a 路径哈希：

```text
user/s<8 位十六进制>.bin   当前进度
user/b<8 位十六进制>.bin   备份
```

结构：文件头（magic、版本、路径哈希、日期、牌组数）+ 每牌组计数 + 每卡 SRS 状态。读取时校验路径哈希，不匹配就忽略，避免不同 apkg 串档。旧版 `user/srs.bin` 只在按文件查找失败时兜底迁移一次。

## 界面

`g_page`：

- 0 文件列表
- 1 牌组列表
- 2 学习页
- 3 完成页
- 4 统计页
- 5 apkg 操作页（长按文件打开）

触摸用 `zone_add(x, y, w, h, id)` 注册区域，`zone_hit` 命中。学习页：

- 普通卡：id 300 全屏看答案；答案页 401–404 评分
- 逐句卡：700–763 每行（最多 64 行）；760 显示全部
- 350 撤销（大按钮）
- 文件列表 100–123 点击打开，长按进入 apkg 操作页 800–802
- 牌组列表 600–663 点击进入；不响应长按删除

绘制按屏幕实际宽高布局，所有界面文字统一用 TITLE 字号；每行字数由运行时屏幕宽度和 `bobo_text_width` 的实际全角字宽计算（S31 约 21 字，含标点），右侧再预留两个全角字的实测像素宽度，避免最后一个字出界，长内容用 `draw_wrapped` 自动换行并按行滚动（每次 3 行，上限 240）。字段清洗缓冲按笔记字段长度动态分配，不再受固定 512/1536 字节限制。逐句卡按 `g_scroll` 从对应行开始画，长诗可上下滑动看后面的句子。
