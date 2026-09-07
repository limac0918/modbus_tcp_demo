#!/usr/bin/env bash
# ============================================================
# release.sh — 一键发版：打 tag + 双推 GitHub / Gitee
#
# 用法：
#   ./release.sh            # 自动按上一个 tag 递增次版本（v1.0.0 → v1.1.0）
#   ./release.sh v1.2.0     # 手动指定版本号
#
# 前置条件：
#   1. 已配置两个远端：github、gitee（git remote add ...）
#   2. 工作区干净（未提交的修改会阻止发版）
#   3. 已切换到要发版的分支（默认 main）
# ============================================================
set -euo pipefail

# ---- 1. 检查远端是否配置 --------------------------------------
for remote in github gitee; do
  if ! git remote | grep -qx "$remote"; then
    echo "❌ 未配置远端：$remote（请先执行 git remote add $remote <仓库地址>）"
    exit 1
  fi
done

# ---- 2. 工作区必须干净 -----------------------------------------
if [ -n "$(git status --porcelain)" ]; then
  echo "❌ 工作区有未提交的修改，请先提交："
  git status --short
  exit 1
fi

# ---- 3. 确定版本号 ----------------------------------------------
LAST_TAG=$(git tag --sort=-v:refname | head -n 1 || true)
if [ -n "${1:-}" ]; then
  VERSION="$1"
  case "$VERSION" in
    v[0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "❌ 版本号格式应为 v主.次.补丁，例如 v1.1.0"; exit 1 ;;
  esac
else
  if [ -z "$LAST_TAG" ]; then
    VERSION="v1.0.0"
  else
    VERSION=$(echo "$LAST_TAG" | awk -F. '{printf "v%d.%d.0", substr($1,2)+0, $2+1}')
    echo "ℹ️  未指定版本号，按上个 tag（$LAST_TAG）递增次版本 → $VERSION"
  fi
fi

if git rev-parse -q --verify "refs/tags/$VERSION" >/dev/null; then
  echo "❌ tag $VERSION 已存在，请检查版本号"
  exit 1
fi

# ---- 4. 发版前确认 ----------------------------------------------
BRANCH=$(git symbolic-ref --short HEAD)
echo "=============================================="
echo "即将发布：$VERSION"
echo "当前分支：$BRANCH"
echo "推送目标：github + gitee（--tags）"
echo "=============================================="
read -r -p "确认？[y/N] " answer
case "$answer" in
  y|Y|yes|YES) ;;
  *) echo "已取消"; exit 0 ;;
esac

# ---- 5. 打 tag 并双推 -------------------------------------------
git tag -a "$VERSION" -m "release: $VERSION"
git push github "$BRANCH" --tags
git push gitee  "$BRANCH" --tags

# ---- 6. 发版完成提示 ---------------------------------------------
echo ""
echo "✅ $VERSION 已发布并同步到 GitHub + Gitee"
echo "自上个版本以来的提交（供更新 CHANGELOG 参考）："
if [ -n "$LAST_TAG" ]; then
  git log --oneline "$LAST_TAG"..HEAD || true
else
  git log --oneline | head -n 10 || true
fi
echo ""
echo "别忘了：更新 CHANGELOG.md 与 README 版本对照表"
