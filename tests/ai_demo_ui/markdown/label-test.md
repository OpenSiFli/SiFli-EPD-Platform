# 长段 label 和 span 对照

在仓库根目录运行以下命令。Windows 使用 WSL Ubuntu。主机测试通过 CMake、Ninja
和 GCC 构建真实 LVGL，启用 ASan 和 UBSan。

    python tests/ai_demo_ui/markdown/label_run.py --output <输出目录>

默认生成一份中英、标点和软换行混合的长段。可用
`--record <已解码会话 JSON>` 导入已有夹具。JSON 中须有 `crc_valid=true` 以及
完整的助手正文。该值由会话记录的解码器提供，测试直接使用其中的原始正文。
未合入候选时，可用 `--renderer-patch <补丁>` 在临时副本上应用。

同一份渲染源码分别关闭和开启 label 路径，比较两个宽度的高度及首、中、尾
像素，并检查富文本、链接、分配失败、视图回滚和销毁。日志保存在指定输出目录。
