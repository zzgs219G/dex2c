#!/usr/bin/env bash
# make-zip.sh — 本地快速打包（**开发测试用，不是发布工具**）
#
# ⚠️⚠️ 正式发布必须走 CI，不要用本脚本产出的包去替换 tools/dex2c.zip。
#
#   正式发布流程（.github/workflows/release.yml）：
#     git commit && git tag vX.Y.Z && git push origin main --tags
#     → CI 跑 bind 冒烟测试(compileall + dcc.py --help)
#     → rsync 打包 + 逐项校验 zip 内容
#     → 创建 GitHub Release，产出 Dex2C-vX.Y.Z.zip
#     → 把该 Release 资产改名成 tools/dex2c.zip
#
#   本脚本的用途只有一个：改引擎后想**立刻**在本地验证产物行为/体积，
#   不用等 CI。它【没有】CI 的冒烟测试与 zip 校验，产出的包不能当发布物。
#
# 为什么不直接用 release.yml 的 rsync 命令做本地测试：
#   release.yml 用 rsync 排除法，而本地工作区有 proj/、__pycache__/、
#   build-tools/、platform-tools/ 这些测试残留，排除法一旦漏项就会把
#   9.5MB 的包打成 28MB（实际发生过）。本脚本改用【白名单】，从根上杜绝。
#
# 用法：
#   ./make-zip.sh                      # 版本号取 git describe
#   ./make-zip.sh v1.0.3-dev           # 显式指定（本地测试建议加 -dev 后缀）
#   ./make-zip.sh -o /path/out.zip     # 指定输出路径
#
# 包含什么（白名单）：
#   androguard/        androguard 依赖（APK/DEX/AXML 解析）
#   dex2c/             转译核心（compiler/writer/graph/...）
#   docs/              文档
#   keystore/          debug 签名证书
#   loader/            DccApplication 模板 smali
#   project/           ★ NDK 工程模板（jni/Android.mk、jni/nc/*.cpp、Application.mk）
#   tools/             apkeditor / apksigner / zipalign 等
#   dcc.py dcc.cfg filter.txt requirements.txt README.md README.txt LICENSE
#   VERSION .gitignore termux_install.sh
#
# ★ project/ 是产物体积的关键：jni/nc/Dex2C.cpp 的解析缓存实现、
#   jni/Application.mk 的 APP_STL 都在这里，改引擎必同步本包重打。
#
# 校验：打完后自动比对文件清单，缺项直接失败（不产出可疑的包）。
set -euo pipefail
cd "$(dirname "$0")"

OUT=""
VER=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -o) OUT="$2"; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    -*) echo "❌ 未知参数: $1" >&2; exit 2 ;;
    *)  VER="$1"; shift ;;
  esac
done

[[ -n "$VER" ]] || VER=$(git describe --tags --always 2>/dev/null || echo dev)
[[ -n "$OUT" ]] || OUT="$(pwd)/dist/dex2c-$VER.zip"
mkdir -p "$(dirname "$OUT")"

# ── 白名单（目录）──────────────────────────────────────────────────
DIRS=(androguard dex2c docs keystore loader project tools)
# ── 白名单（根目录文件）────────────────────────────────────────────
FILES=(dcc.py dcc.cfg filter.txt requirements.txt README.md README.txt LICENSE
       VERSION .gitignore termux_install.sh)

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/Dex2C"
missing=()
for d in "${DIRS[@]}"; do
  [[ -d "$d" ]] || { missing+=("$d/"); continue; }
  cp -r "$d" "$STAGE/Dex2C/"
done
for f in "${FILES[@]}"; do
  if [[ -f "$f" ]]; then
    cp "$f" "$STAGE/Dex2C/"
  else
    # VERSION 不在 git 里，历史上是手工放进去的 —— 缺了用 git describe 补
    if [[ "$f" == VERSION ]]; then
      echo "$VER" > "$STAGE/Dex2C/VERSION"
    else
      missing+=("$f")
    fi
  fi
done
if [[ ${#missing[@]} -gt 0 ]]; then
  echo "❌ 白名单文件缺失: ${missing[*]}" >&2
  exit 1
fi

# 清构建残留（__pycache__ / *.pyc），它们会让包体积无谓膨胀且随 Python 版本变化
find "$STAGE/Dex2C" -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null || true
find "$STAGE/Dex2C" -name "*.pyc" -delete 2>/dev/null || true
# 本地测试目录（dcc.py 的产物落点），绝不能进分发
rm -rf "$STAGE/Dex2C/proj" "$STAGE/Dex2C/.tmp" 2>/dev/null || true

# ── 关键产物断言：引擎改了却没重打包，这里会红 ──────────────────────
for need in project/jni/Android.mk project/jni/Application.mk \
            project/jni/nc/Dex2C.cpp project/jni/nc/Dex2C.h \
            project/jni/nc/well_known_classes.cpp dcc.py; do
  [[ -f "$STAGE/Dex2C/$need" ]] || { echo "❌ 分发包缺关键文件: $need" >&2; exit 1; }
done

rm -f "$OUT"
( cd "$STAGE" && zip -qr "$OUT" Dex2C )
n=$(unzip -l "$OUT" | awk '$4!="" && $4!="Name" && $4!~/\/$/' | wc -l)
sz=$(stat -c %s "$OUT")
echo "✅ $OUT"
echo "   版本: $VER"
echo "   文件: $n 个"
echo "   大小: $sz 字节 ($(awk -v b=$sz 'BEGIN{printf "%.1f", b/1048576}') MB)"
