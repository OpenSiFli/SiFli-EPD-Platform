# libmobi 私有移植

版本：`configure.ac` 声明 libmobi 0.12。
源码基线：SiFli solution2.0 的 `sdk/external/libmobi`，以文件复制保留供应商源码、
版权头、许可证和样本。这里只构建 `codecs/mobi/SConscript` 列出的解析源文件。

本地补丁：

- `src/debug.h`：将内存与 DFS 操作定向至模块适配层。
- `src/mobi.h`：移除 LVGL v8 类型依赖；增加真实解压记录边界、记录查找游标和 HUFF/CDIC 字典缓存。
- `src/memory.c`：释放记录边界与字典缓存，销毁记录时清空查找游标。
- `src/parse_rawml.c`：修复按固定长度推算记录偏移造成的 KF8/Huffman 章节拼接错误。
- `src/read.c`：传播 Record0 分配失败，防止空 MOBI 头继续参与解析；按需记录通过模块预读接口读取。
- `src/parse_rawml.c` 的资源索引只读取前 8 字节，保留完整记录大小用于类型检查。
- `src/util.c`：懒加载兼容的 Print Replica 检查、hybrid 资源头空指针检查、
  禁用文件转储和嵌入字体解码；顺序记录查找复用游标，逐记录解压复用 HUFF/CDIC 字典。
- `src/util.h`：本构建不引入未使用的 zlib 字体依赖。

许可证原文：`COPYING` 为 LGPLv3；`COPYING.GPL3` 为其引用的 GPLv3 全文，
文本来源为 SiFli solution2.0 的
`sdk/external/cairo-latest/cairo-1.14.12/util/cairo-trace/COPYING-GPL-3`。
该复制只补齐许可证文本，没有引入 Cairo 代码。
