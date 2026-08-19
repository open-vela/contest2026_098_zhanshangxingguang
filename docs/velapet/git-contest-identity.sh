#!/usr/bin/env bash
# git-contest-identity.sh
# 在当前 git 仓库里把提交身份切成 openvela 大赛身份(Zhang Yan / gmail)。
#
# 用法:
#   在某个仓库目录内运行:
#     bash git-contest-identity.sh          # 若检测到 open-vela 远程则自动设置
#     bash git-contest-identity.sh --force   # 无论远程是什么都设置
#     bash git-contest-identity.sh --show     # 只显示当前身份, 不改
#
# 说明:只改"当前仓库"的 local 配置(.git/config),不动全局默认(小米身份)。
# 注:大赛目录(contest_submit / vendor/openvela / ~/contest_ws / ~/openvela)已由
#     ~/.gitconfig 的 includeIf 自动切换,通常无需手动跑此脚本;本脚本用于其它位置的仓库。

set -e

CONTEST_NAME="Zhang Yan"
CONTEST_EMAIL="yz471686525@gmail.com"

# 必须在 git 仓库内
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "[x] 当前目录不是 git 仓库。请在仓库目录内运行。"
  exit 1
fi

show() {
  echo "    仓库: $(git rev-parse --show-toplevel)"
  echo "    当前 user.name  = $(git config user.name || echo '(未设)')"
  echo "    当前 user.email = $(git config user.email || echo '(未设)')"
}

if [ "$1" = "--show" ]; then
  echo "[i] 当前身份:"; show; exit 0
fi

FORCE=0
[ "$1" = "--force" ] && FORCE=1

# 检测远程里有没有 open-vela
IS_CONTEST=0
if git remote -v 2>/dev/null | grep -qiE "github.com[:/]open-vela"; then
  IS_CONTEST=1
fi

if [ "$FORCE" = "1" ] || [ "$IS_CONTEST" = "1" ]; then
  git config user.name  "$CONTEST_NAME"
  git config user.email "$CONTEST_EMAIL"
  echo "[✓] 已把本仓库身份设为大赛身份:"
  show
  echo "    提交时记得用 'git commit -s'(带 Signed-off-by),message 保持英文。"
else
  echo "[i] 未检测到 open-vela 远程,未改动(用 --force 可强制设置)。"
  show
fi
