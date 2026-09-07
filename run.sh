#!/bin/bash
# =====================================================================
# WSL 启动脚本（构建 + 安装到 ~/.local/bin + 运行）
# ---------------------------------------------------------------------
# 背景：WSLg 的 XWayland -> weston 渲染链路在本机异常
#       （窗口能创建但内容不渲染到 Windows 桌面，任务栏只有图标）。
#       强制 Qt 走 Wayland 原生协议可绕开该链路，正常显示。
# 在真实 Linux 桌面(X11)下运行可注释掉下面这行，或改为 xcb。
#
# 【环境依赖】
#   1) CMAKE_PREFIX_PATH → 构建期 cmake 找 Qt（.bashrc 已配置）
#   2) LD_LIBRARY_PATH   → 运行期找 aqtinstall 的 Qt 库
#      · 若 .bashrc 已配置 → 直接生效
#      · 若未生效（本次报错场景）→ 下方自动兜底前置添加
#
# 【安装位置】CMAKE_INSTALL_PREFIX=$HOME/.local，最终产物
#   ~/.local/bin/modbus_tcp_demo
# =====================================================================
set -e
cd "$(dirname "$0")"
export QT_QPA_PLATFORM=wayland
# 若之前手动 export 过调试变量，清掉避免刷屏
unset QT_DEBUG_PLUGINS 2>/dev/null || true

# ---- 运行期 Qt 库路径兜底（bashrc 未配置/未生效时保证能找到）----
QT_LIB="$HOME/Qt/5.15.2/gcc_64/lib"
if [ -d "$QT_LIB" ]; then
    case ":$LD_LIBRARY_PATH:" in
        *":$QT_LIB:"*) : ;;                                  # 已包含 → 不动
        *) export LD_LIBRARY_PATH="$QT_LIB:$LD_LIBRARY_PATH" ;; # 未包含 → 前置添加
    esac
fi

INSTALL_PREFIX="$HOME/.local"
BIN="$INSTALL_PREFIX/bin/modbus_tcp_demo"
# ---- 每次都 配置 + 构建 + 覆盖安装 ----
# 增量构建：源码没改动时 cmake --build 秒过（不重编译），install 只是覆盖复制，
# 改代码后直接 ./run.sh 即自动生效，无需手动删旧二进制。
echo "== 配置 =="
echo "CMAKE_INSTALL_PREFIX=$INSTALL_PREFIX | LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
cmake -B build -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX"
echo "== 构建 =="
cmake --build build -j
echo "== 覆盖安装到 $BIN =="
cmake --install build
exec "$BIN"
