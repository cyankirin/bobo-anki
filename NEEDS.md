# 需要但当前平台没有提供的能力

## 1. zstd 解压

Anki 2.1.50 之后默认导出的 `.apkg`，数据库是 `collection.anki21b`，用 zstd 压缩；`media` 映射文件也可能是 zstd。
设备的宿主接口没有 zstd，应用也不能自带第三方解压库之外的二进制（且 ELF 只能导入 `bobo_*` 与 libc 子集）。

- 现在的变通：检测到 `collection.anki21b` 时提示用户用 Anki「导出 › 支持旧版 Anki」重新导出，得到 `collection.anki2`。
- 需要的签名：`int bobo_zstd_decompress(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len);`

## 2. 卡组媒体（图片、音频）

apkg 里的 `media` 是 zip 内的另一份归档，Lua 与 ELF 都没有读取包内条目的接口；ELF 只能读 TF 卡上的独立文件。

- 现在的变通：只显示文字，图片与音频跳过。
- 需要的签名：`int bobo_zip_read_memory(const void* zip, size_t zip_len, const char* name, void* out, size_t cap);`

## 3. 更细的进度文件接口

现在整份进度（最多 12000 张 × 24 字节）每次保存整文件写一遍；卡组很大时每次评分写盘约 300 KB。
如果能按块读写，可以只写改动的部分。

- 需要的签名：`bool bobo_file_write_range(const char* name, uint32_t offset, const void* data, size_t size);`
