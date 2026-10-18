#!/bin/sh
# 极简 pkg-config 替身 —— FFmpeg / mpv(waf) 的 configure 都把它当 pkg-config 用。
#
# 为什么需要：宿主根本没装 pkg-config/pkgconf，而
#   * FFmpeg 的 `require_pkg_config libxml2 libxml-2.0 ...` 是硬闸门，
#     拿不到 --exists/--cflags/--libs 就直接 die；
#   * mpv 的 waf 用 check_cfg 问 libass / 六个 FFmpeg 包。
# 反过来也不能装一个宿主 pkg-config：它会捞到宿主 x86_64 的 .pc，
# 编译期"找到了"、链接期才炸（与 FREETYPE_* 那套坑同源）。
# 所以搜索路径被 PKG_CONFIG_LIBDIR 钉死成 Xbox 自己那几份安装树。
#
# 必须支持的调用形状（缺一个 waf 就跑不过，全部来自源码）：
#   waftools/checks/generic.py::_check_pkg_config 会拼出
#     pkg-config 'libass >= 0.12.2' --libs --cflags
#     pkg-config 'libavutil >= 56.70.100' 'libavcodec >= 58.134.100' … --libs --cflags
#   —— 版本约束与包名在**同一个 argv** 里、多包一次问、两个模式要**合并输出**；
#   exec_cfg 还可能追加 --static / --variable=X / --define-variable=k=v。
# FFmpeg configure 用 --exists / --print-errors / --modversion /
#   --atleast-version= / --cflags / --libs（各一次、单包）。
#
# 与真 pkg-config 的差异（本项目范围内可接受）：
#   * Requires / Requires.private 会递归展开，但**找不到的 .pc 容错跳过**
#     —— libass.pc 声明 Requires: …, fribidi …，而 libfribidi 是纯静态归档、
#     没有 .pc；最终链接由 tests/p4_libmpv/Makefile 显式排归档解决；
#   * 只做"整串去重"，不区分 -l/-L 的重复语义；
#   * 未知开关一律忽略：宁可多认，也不让 configure 卡在用不到的旗标上。
#
# 搜索路径：$PKG_CONFIG_LIBDIR（冒号分隔，优先）→ $PKG_CONFIG_PATH → 标准目录。
set -u

NL='
'

# ---------- 搜索路径 ----------
dirs=${PKG_CONFIG_LIBDIR:-/usr/local/lib/pkgconfig:/usr/lib/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig}
if [ -n "${PKG_CONFIG_PATH:-}" ]; then
    dirs="$dirs:$PKG_CONFIG_PATH"
fi

# ---------- 模式 ----------
mode=flags        # flags | exists | version | vcheck | variable | pcversion
mode_set=0
do_cflags=0
do_libs=0
do_static=0
print_errors=1
want_ver=
ver_op='>='
want_var=
ovr=""
queue=            # 待处理记录：name|op|ver|src(user|req)
pending_name=
pending_ver=

set_mode() {
    mode=$1
    mode_set=1
}

# 把"带空格的单个 argv"拆进队列：'name' 或 'name >= 1.0'
# 必须是函数：POSIX sh 的 `set --` 会**改写当前位置参数**，若在主解析循环里直接
# set -- $arg，外层 while 后续要读的参数（后面的包名、--libs/--cflags）会被整个顶掉
# —— 表现为"六包一次问只返回第一个包"。函数有自己的一份位置参数，改不到外层。
enqueue_space_arg() {
    set -- $1
    case $# in
        0) ;;
        1) queue="$queue$1|||user$NL" ;;
        *) queue="$queue$1|$2|${3:-}|user$NL" ;;
    esac
}

while [ $# -gt 0 ]; do
    arg=$1
    shift
    case $arg in
        --version)
            printf '0.29.2\n'
            exit 0
            ;;
        --atleast-pkgconfig-version=*)
            # 0.29.2 >= 要求值 才算过（FFmpeg 会问 0.9.3）
            want=${arg#*=}
            if [ "$(printf '%s\n%s\n' "$want" "0.29.2" | sort -V | head -n1)" = "$want" ]; then
                exit 0
            fi
            exit 1
            ;;
        --exists)        set_mode exists ;;
        --modversion)    set_mode version ;;
        --cflags|--cflags-only-I|--cflags-only-other)
            do_cflags=1; set_mode flags ;;
        --libs|--libs-only-L|--libs-only-l|--libs-only-other)
            do_libs=1; set_mode flags ;;
        --static|--static-libs)
            do_static=1 ;;
        --variable=*)
            want_var=${arg#*=}; set_mode variable ;;
        --define-variable=*)
            ovr="$ovr ${arg#*=}" ;;
        --atleast-version=*)
            want_ver=${arg#*=}; ver_op='>='; set_mode vcheck ;;
        --exact-version=*)
            want_ver=${arg#*=}; ver_op='='; set_mode vcheck ;;
        --max-version=*)
            want_ver=${arg#*=}; ver_op='<='; set_mode vcheck ;;
        --print-errors)  print_errors=1 ;;
        --silence-errors) print_errors=0 ;;
        --*)             : ;;   # 未知开关：忽略
        '='|'>'|'>='|'<'|'<='|'!=')
            # 分开写的版本约束：pkg >= 1.0
            queue="$queue$pending_name|$arg|${1:-}|user$NL"
            [ $# -gt 0 ] && shift
            pending_name=
            ;;
        *)
            # 单个 argv 里可能是 "name"、也可能是 "name >= 1.0"
            case $arg in
                *' '*)
                    if [ -n "$pending_name" ]; then
                        queue="$queue$pending_name|||user$NL"
                        pending_name=
                    fi
                    enqueue_space_arg "$arg"
                    ;;
                *)
                    # 可能是 "pkg >= 1.0" 拆开写的前半截，先攒着
                    if [ -n "$pending_name" ]; then
                        queue="$queue$pending_name|||user$NL"
                    fi
                    pending_name=$arg
                    ;;
            esac
            ;;
    esac
done

if [ -n "$pending_name" ]; then
    queue="$queue$pending_name|||user$NL"
fi

if [ -z "$queue" ]; then
    echo "pkg-config: no package name given" >&2
    exit 1
fi

# 没有任何显式模式时与真 pkg-config 一致：打印 --cflags --libs
if [ "$mode_set" -eq 0 ]; then
    mode=flags
    do_cflags=1
    do_libs=1
elif [ "$mode" = flags ]; then
    [ "$do_cflags" -eq 1 ] || [ "$do_libs" -eq 1 ] || { do_cflags=1; do_libs=1; }
fi

err() {
    [ "$print_errors" -eq 1 ] || return 0
    printf '%s\n' "$*" >&2
}

# $1=版本  $2=运算符  $3=要求版本 → 0 表示满足
ver_ok() {
    case $2 in
        '=')  [ "$1" = "$3" ] && return 0 ;;
        '>')  [ "$(printf '%s\n%s\n' "$3" "$1" | sort -V | head -n1)" = "$3" ] && [ "$1" != "$3" ] && return 0 ;;
        '>=') [ "$(printf '%s\n%s\n' "$3" "$1" | sort -V | head -n1)" = "$3" ] && return 0 ;;
        '<')  [ "$(printf '%s\n%s\n' "$1" "$3" | sort -V | head -n1)" = "$1" ] && [ "$1" != "$3" ] && return 0 ;;
        '<=') [ "$(printf '%s\n%s\n' "$1" "$3" | sort -V | head -n1)" = "$1" ] && return 0 ;;
        '!=') [ "$1" != "$3" ] && return 0 ;;
        *)    return 0 ;;
    esac
    return 1
}

find_pc() {
    _old_ifs=$IFS
    IFS=:
    for _d in $dirs; do
        IFS=$_old_ifs
        [ -n "$_d" ] || continue
        if [ -f "$_d/$1.pc" ]; then
            printf '%s\n' "$_d/$1.pc"
            return 0
        fi
    done
    IFS=$_old_ifs
    return 1
}

# 展开一个 .pc：变量代换 + ${} 递归 + --define-variable 覆盖，
# 输出 "<字段>\t<值>"（字段含 Version/Cflags/Libs/Requires/… 与 VAR:<名>）。
expand_pc() {
    awk -v OVR="$ovr" '
function trim(s) {
    sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s
}
# 把 ${name} 换成 vars[name]（已展开的）；递归调 match() 会覆盖 RSTART/RLENGTH，
# 所以先存到局部 rs/rl（否则 substr 削不掉片段 → 同一个 ${…} 反复匹配 → 死循环）。
function expand(s, depth,   out, key, val, rs, rl) {
    if (depth > 10) return s
    out = ""
    while (match(s, /\$\{[A-Za-z_][A-Za-z_0-9]*\}/)) {
        rs = RSTART; rl = RLENGTH
        out = out substr(s, 1, rs - 1)
        key = substr(s, rs + 2, rl - 3)
        val = (key in vars) ? vars[key] : ""
        out = out expand(val, depth + 1)
        s = substr(s, rs + rl)
    }
    return out s
}
{
    line = $0
    while (line ~ /\\[ \t]*$/) {           # 行尾反斜杠续行
        sub(/\\[ \t]*$/, "", line)
        if ((getline nxt) > 0) line = line nxt; else break
    }
    line = trim(line)
    if (line == "" || line ~ /^#/) next
    c = index(line, ":"); e = index(line, "=")
    if (c == 0 && e == 0) next
    if (e > 0 && (c == 0 || e < c)) {      # 变量：prefix=/x
        key = trim(substr(line, 1, e - 1))
        raw_var[++nvar] = key
        var_val[key] = trim(substr(line, e + 1))
    } else {                               # 字段：Cflags: -I/x
        key = trim(substr(line, 1, c - 1))
        raw_field[++nfield] = key
        field_val[key] = trim(substr(line, c + 1))
    }
}
END {
    # --define-variable=k=v 先于展开生效
    n = split(OVR, toks, " ")
    for (i = 1; i <= n; i++) {
        p = index(toks[i], "=")
        if (p > 1) var_val[trim(substr(toks[i], 1, p - 1))] = substr(toks[i], p + 1)
    }
    for (i = 1; i <= nvar; i++) {
        k = raw_var[i]
        if (!(k in expanded)) { vars[k] = expand(var_val[k], 0); expanded[k] = 1 }
    }
    # 没写 exec_prefix/libdir/includedir 的 .pc 用 pkg-config 的缺省值
    if (!("prefix" in vars))       { vars["prefix"] = "/usr/local"; expanded["prefix"] = 1 }
    if (!("exec_prefix" in vars))  { vars["exec_prefix"] = "${prefix}"; expanded["exec_prefix"] = 1 }
    if (!("libdir" in vars))       { vars["libdir"] = "${exec_prefix}/lib"; expanded["libdir"] = 1 }
    if (!("includedir" in vars))   { vars["includedir"] = "${prefix}/include"; expanded["includedir"] = 1 }
    for (k in vars) printf "VAR:%s\t%s\n", k, expand(vars[k], 0)
    for (i = 1; i <= nfield; i++) {
        k = raw_field[i]
        printf "%s\t%s\n", k, expand(field_val[k], 0)
    }
}' "$1"
}

# 从展开结果里取一个字段
field_of() {
    printf '%s\n' "$2" | awk -F'\t' -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }'
}

out_cflags=
out_libs=
seen="|"
status=0
results=          # 每个 user 包一行：name<TAB>version<TAB>var值
first_var_done=0

append_tok() {
    _which=$1
    shift
    for _t in "$@"; do
        [ -n "$_t" ] || continue
        if [ "$_which" = c ]; then _cur=$out_cflags; else _cur=$out_libs; fi
        case " $_cur " in
            *" $_t "*) ;;
            *) if [ "$_which" = c ]; then out_cflags="$_cur $_t"
               else out_libs="$_cur $_t"; fi ;;
        esac
    done
}

while [ -n "$queue" ]; do
    rec=${queue%%"$NL"*}
    queue=${queue#*"$NL"}
    name=${rec%%|*}
    r1=${rec#*|}
    op=${r1%%|*}
    r2=${r1#*|}
    ver=${r2%%|*}
    src=${r2#*|}
    [ -n "$name" ] || continue
    case $seen in *"|$name|"*) continue ;; esac
    seen="$seen$name|"

    pc=$(find_pc "$name") || pc=
    if [ -z "$pc" ]; then
        if [ "$src" = req ]; then
            continue          # 残缺的 Requires：容错跳过（见文件头注释）
        fi
        err "Package $name was not found in the pkg-config search path."
        err "Searched in: $(printf '%s' "$dirs" | tr ':' ' ')"
        status=1
        continue
    fi

    data=$(expand_pc "$pc")
    have=$(field_of Version "$data")
    if [ -n "$op" ]; then
        # 记录里带着内联约束："libavutil >= 56.70.100"
        if ! ver_ok "$have" "$op" "$ver"; then
            err "Package $name was found with version $have, but $op $ver is required."
            status=1
            continue
        fi
    elif [ "$mode" = vcheck ] && [ "$src" = user ] && [ -n "$want_ver" ]; then
        # --atleast-version= / --exact-version= / --max-version=
        if ! ver_ok "$have" "$ver_op" "$want_ver"; then
            err "Package $name was found with version $have, but $ver_op $want_ver is required."
            status=1
            continue
        fi
    fi

    if [ "$do_cflags" -eq 1 ]; then
        # shellcheck disable=SC2046
        append_tok c $(field_of Cflags "$data")
    fi
    if [ "$do_libs" -eq 1 ]; then
        # shellcheck disable=SC2046
        append_tok l $(field_of Libs "$data")
        if [ "$do_static" -eq 1 ]; then
            # shellcheck disable=SC2046
            append_tok l $(field_of 'Libs.private' "$data")
        fi
    fi

    varval=$(field_of "VAR:$want_var" "$data")
    # 只记录用户直接点名的包：Requires 递归进来的包不进 results，
    # 否则 `pkg-config --modversion libass` 会把 harfbuzz/freetype 的版本也打出来
    if [ "$src" = user ]; then
        results="$results$name|$have|$varval
"
    fi

    # Requires 递归（进队列，不用函数递归 —— POSIX sh 没有 local）
    reqs=$(field_of Requires "$data")
    if [ "$do_static" -eq 1 ]; then
        reqs="$reqs $(field_of 'Requires.private' "$data")"
    fi
    reqs=$(printf '%s' "$reqs" | tr ',' '\n')
    while [ -n "$reqs" ]; do
        line=${reqs%%"$NL"*}
        case $reqs in
            *"$NL"*) reqs=${reqs#*"$NL"} ;;
            *)       reqs= ;;
        esac
        set -- $line
        case $# in
            0) ;;
            1) queue="$queue$1|||req$NL" ;;
            *) queue="$queue$1|$2|${3:-}|req$NL" ;;
        esac
    done
done

case $mode in
    exists)
        exit $status
        ;;
    version)
        [ "$status" -eq 0 ] || exit $status
        rest=$results
        while [ -n "$rest" ]; do
            r=${rest%%"$NL"*}
            case $rest in *"$NL"*) rest=${rest#*"$NL"} ;; *) rest= ;; esac
            [ -n "$r" ] || continue
            v=${r#*|}; v=${v%%|*}
            printf '%s\n' "$v"
        done
        exit 0
        ;;
    variable)
        [ "$status" -eq 0 ] || exit $status
        rest=$results
        while [ -n "$rest" ]; do
            r=${rest%%"$NL"*}
            case $rest in *"$NL"*) rest=${rest#*"$NL"} ;; *) rest= ;; esac
            [ -n "$r" ] || continue
            v=${r#*|}; v=${v#*|}
            printf '%s\n' "$v"
        done
        exit 0
        ;;
    vcheck)
        [ "$status" -eq 0 ] || exit $status
        # status 已在版本不匹配处置 1；这里再兜一次
        exit 0
        ;;
    pcversion)
        exit 0
        ;;
    flags)
        [ "$status" -eq 0 ] || exit $status
        out=$(printf '%s %s' "$out_cflags" "$out_libs" | sed -e 's/  */ /g' -e 's/^ //' -e 's/ $//')
        [ -n "$out" ] && printf '%s\n' "$out"
        exit 0
        ;;
esac
exit $status
