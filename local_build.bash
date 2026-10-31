#!/usr/bin/env bash
#
# ANGLE 本地/交叉构建（交互式）
# 平台：android / ios / linux
# 架构：arm64 / arm32 / x86 / x86_64
# 模式：release / debug / fastdebug（fastdebug 仅 android-arm64）
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "======================================"
echo "     ANGLE 本地构建脚本"
echo "======================================"
echo ""

# ---------- 平台 ----------
echo "选择平台:"
echo "  1) android"
echo "  2) ios  (需要 macOS + Xcode)"
echo "  3) linux"
read -rp "输入编号 [1]: " PLATFORM_NUM
PLATFORM_NUM="${PLATFORM_NUM:-1}"
case "$PLATFORM_NUM" in
  1) PLATFORM=android ;;
  2) PLATFORM=ios ;;
  3) PLATFORM=linux ;;
  *) echo "无效选项"; exit 1 ;;
esac

# ---------- 架构 ----------
case "$PLATFORM" in
  android)
    echo ""
    echo "选择 Android 架构:"
    echo "  1) arm64"
    echo "  2) arm32"
    echo "  3) x86"
    echo "  4) x86_64"
    read -rp "输入编号 [1]: " ARCH_NUM
    ARCH_NUM="${ARCH_NUM:-1}"
    case "$ARCH_NUM" in
      1) ARCH=arm64 ;;
      2) ARCH=arm32 ;;
      3) ARCH=x86 ;;
      4) ARCH=x86_64 ;;
      *) echo "无效选项"; exit 1 ;;
    esac
    ;;
  ios)
    ARCH=arm64
    echo ""
    echo "iOS 架构固定为 arm64"
    ;;
  linux)
    ARCH=x86_64
    echo ""
    echo "Linux 架构固定为 x86_64"
    ;;
esac

# ---------- 模式 ----------
echo ""
echo "选择构建模式:"
echo "  1) release"
echo "  2) debug"
if [ "$PLATFORM" = "android" ] && [ "$ARCH" = "arm64" ]; then
  echo "  3) fastdebug  (仅 android-arm64)"
  read -rp "输入编号 [1]: " MODE_NUM
  MODE_NUM="${MODE_NUM:-1}"
  case "$MODE_NUM" in
    1) MODE=release ;;
    2) MODE=debug ;;
    3) MODE=fastdebug ;;
    *) echo "无效选项"; exit 1 ;;
  esac
else
  read -rp "输入编号 [1]: " MODE_NUM
  MODE_NUM="${MODE_NUM:-1}"
  case "$MODE_NUM" in
    1) MODE=release ;;
    2) MODE=debug ;;
    *) echo "无效选项"; exit 1 ;;
  esac
fi

# ---------- out-dir ----------
OUT_DIR="${PLATFORM}-${ARCH}"
if [ "$MODE" != "release" ]; then
  OUT_DIR="${OUT_DIR}-${MODE}"
fi

echo ""
echo "构建配置:"
echo "  平台:   $PLATFORM"
echo "  架构:   $ARCH"
echo "  模式:   $MODE"
echo "  输出:   out/$OUT_DIR"
echo ""
read -rp "是否继续? [Y/n]: " CONFIRM
if [[ "$CONFIRM" =~ ^[Nn] ]]; then
  echo "已取消"
  exit 0
fi

# ---------- 环境 ----------
export PATH="$SCRIPT_DIR/depot_tools:$PATH"

if [ ! -d "$SCRIPT_DIR/depot_tools" ]; then
  echo ">>> 未找到 depot_tools，正在克隆..."
  git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
fi

if [ ! -f "$SCRIPT_DIR/.gclient" ]; then
  echo ">>> 初始化 gclient..."
  python3 scripts/bootstrap.py
fi

echo ">>> gclient sync ..."
gclient sync --no-history --shallow --reset --force

if [ -f angle-patches.diff ]; then
  echo ">>> 应用补丁 ..."
  git apply --verbose angle-patches.diff || true
fi

# ---------- 生成 args.gn ----------
echo ">>> 生成 args.gn ..."
bash scripts/gen_args.sh "$PLATFORM" "$ARCH" "$MODE" "$OUT_DIR"

# ---------- GN ----------
cp .gn .gn.bak 2>/dev/null || true
if grep -q 'expand_directory_allowlist' .gn 2>/dev/null; then
  sed -i 's|]|,\n  "//build/modules/BUILD.gn"\n]|' .gn
else
  echo 'expand_directory_allowlist = [ "//build/modules/BUILD.gn" ]' >> .gn
fi

echo ">>> gn gen ..."
gn gen "out/$OUT_DIR" --threads=16
gn args "out/$OUT_DIR" --list --short

# ---------- 编译 ----------
echo ">>> 开始编译 ..."
autoninja -C "out/$OUT_DIR" -j 16

echo ""
echo "======================================"
echo "  构建完成: out/$OUT_DIR"
echo "======================================"
find "out/$OUT_DIR" -maxdepth 3 \
  \( -name "*.so" -o -name "*.framework" -o -name "*.dylib" \) \
  2>/dev/null || true
