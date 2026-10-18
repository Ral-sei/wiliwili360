#!/bin/sh
# FFmpeg configure 的假链接器（P2 交叉 configure 专用）。
#
# configure 的所有 check_lib / check_func / check_builtin 都是"编译真实 + 链接
# 测试"两步，链接测试只用来决定 HAVE_* 宏，conftest 从不执行（cross compile）。
# 真正的最终链接由 oxdklink 完成（标题对象 + FFmpeg 静态库 + XDK 导入 thunk，
# 见 OXDK xbox360/tools/oxdklink/oxdklink.py），没有 libc、没有 crt、符号要靠
# 导入 thunk 解析，clang 驱动在这条 target 上还会绕进宿主 gcc 的 collect2
# （elf_i386 混乱，实测无法工作）。
#
# 所以这里:忽略参数，产出 -o 指定的文件，返回成功。
# 代价:configure 可能误判"链接期才存在"的函数为存在（如 lrintf/memalign）。
# 校正:最终 XEX 链接会把每个真缺的符号报出来，再补 compat 实现或关掉特性。
# 只编译不链接的检查（check_header/check_cpp/check_type/inline asm）不受影响，
# 仍然是真实的。

outfile=
prev=
for a in "$@"; do
    if [ "$prev" = "-o" ]; then
        outfile=$a
    fi
    prev=$a
done
[ -n "$outfile" ] && : > "$outfile"
exit 0
