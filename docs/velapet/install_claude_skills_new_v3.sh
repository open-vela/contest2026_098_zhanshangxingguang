#!/usr/bin/env bash
#
# install_claude_skills_new.sh
# 一键安装 openvela AI 大赛开发环境：
#   1) Claude 技能集 (.claude/) 到 openvela 项目根目录
#      公开比赛版：github.com/open-vela/.claude 分支 dev-ai-contest-2026
#   2) (可选) 安装 Claude Code CLI
#   3) (可选) 配置 MiMo 接入（写 ~/.claude/settings.json，含完整模型映射）
#   4) (可选) 连通性自检
#
# 用法：
#   ./install_claude_skills_new.sh [选项] [openvela项目根目录]
#   ./install_claude_skills_new.sh --help
#
# 换新电脑一条命令搞定（把 tp-xxx 换成自己的 key）：
#   ./install_claude_skills_new.sh -y -k tp-xxxxxxxx ~/openvela/vela_ap
#
# MiMo 接入参数依据官方文档：
#   https://mimo.mi.com/docs/zh-CN/tokenplan/integration/claudecode
#
set -euo pipefail

# ============================================================
# 常量与默认值
# ============================================================
REPO_URL="${OPENVELA_SKILLS_REPO:-https://github.com/open-vela/.claude.git}"
BRANCH="${OPENVELA_SKILLS_BRANCH:-dev-ai-contest-2026}"

CLAUDE_DIR="$HOME/.claude"
SETTINGS_FILE="$CLAUDE_DIR/settings.json"
CLAUDE_JSON="$HOME/.claude.json"
LEGACY_ENV_FILE="$HOME/.claude_code_env"   # 旧版脚本留下的环境变量文件

BASE_URL_TOKENPLAN="https://token-plan-cn.xiaomimimo.com/anthropic"
DEFAULT_MODEL="mimo-v2.5-pro"
ANTHROPIC_API_VERSION="2023-06-01"

# 可通过命令行覆盖
PROJECT_ROOT=""
API_KEY="${MIMO_API_KEY:-}"
BASE_URL=""
MODEL="$DEFAULT_MODEL"
USE_1M=0
ASSUME_YES=0
DO_SKILLS=1
DO_CLI=1
DO_CONFIG=1
DO_TEST=1

# ============================================================
# 输出helper
# ============================================================
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  C_OK=$'\033[32m'; C_WARN=$'\033[33m'; C_ERR=$'\033[31m'; C_DIM=$'\033[2m'; C_OFF=$'\033[0m'
else
  C_OK=""; C_WARN=""; C_ERR=""; C_DIM=""; C_OFF=""
fi
step() { printf '\n==> %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
ok()   { printf '    %s%s%s\n' "$C_OK"   "OK: $*"   "$C_OFF"; }
warn() { printf '    %s%s%s\n' "$C_WARN" "警告: $*" "$C_OFF" >&2; }
die()  { printf '\n%s%s%s\n' "$C_ERR" "错误: $*" "$C_OFF" >&2; exit 1; }

# 掩码显示密钥，避免完整 key 出现在终端/日志里
mask_key() {
  local k="$1"
  if [ "${#k}" -le 10 ]; then printf '%s' "***"; else printf '%s...%s' "${k:0:5}" "${k: -3}"; fi
}

# 清理粘贴进来的 key：去首尾空白/回车、去包裹的引号
normalize_key() {
  local s="$1"
  s="${s%$'\r'}"
  s="${s#"${s%%[![:space:]]*}"}"
  s="${s%"${s##*[![:space:]]}"}"
  case "$s" in
    \"*\") s="${s#\"}"; s="${s%\"}" ;;
    \'*\') s="${s#\'}"; s="${s%\'}" ;;
  esac
  printf '%s' "$s"
}

# 转义成 JSON 字符串安全形式（反斜杠和双引号）
json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '%s' "$s"
}

# 安全提问：set -e 下 read 失败(EOF/非交互)不会中断脚本；非交互一律取默认值
# 用法: if ask "要装吗?" y; then ... fi
ask() {
  local prompt="$1" def="${2:-n}" ans="" hint="[y/N]"
  [ "$def" = "y" ] && hint="[Y/n]"
  if [ "$ASSUME_YES" = "1" ]; then
    info "$prompt $hint -> 自动选择 $def (--yes)"
    [ "$def" = "y" ]; return
  fi
  if [ ! -t 0 ]; then
    info "$prompt $hint -> 非交互环境，取默认 $def"
    [ "$def" = "y" ]; return
  fi
  read -r -p "$prompt $hint " ans || ans=""
  ans="${ans:-$def}"
  case "$ans" in [yY]|[yY][eE][sS]) return 0;; *) return 1;; esac
}

usage() {
  cat <<'EOF'
用法: ./install_claude_skills_new.sh [选项] [openvela项目根目录]

选项:
  -r, --root DIR        openvela 项目根目录（也可作为位置参数传入，默认当前目录）
  -k, --api-key KEY     Token Plan 专属 API Key（tp-xxx，控制台"专属 API key"那一栏）
                        也可用环境变量 MIMO_API_KEY 传入，避免出现在 shell 历史里
  -b, --base-url URL    自定义 Anthropic 兼容协议地址
                        默认 https://token-plan-cn.xiaomimimo.com/anthropic
  -m, --model NAME      模型 ID（默认 mimo-v2.5-pro）
      --1m              启用 1M 长上下文（模型 ID 追加 [1m] 后缀）
  -y, --yes             非交互模式，所有可选步骤取默认值（需配合 -k 使用）
      --skip-skills     跳过技能集安装
      --skip-cli        跳过 Claude Code CLI 安装
      --skip-config     跳过接入配置
      --no-test         跳过连通性自检（自检会消耗极少量额度）
  -h, --help            显示本帮助

示例:
  # 只配 Token Plan 专属 API Key（最常用）
  ./install_claude_skills_new.sh --skip-skills --skip-cli -k tp-xxxx

  # key 走环境变量，不进 shell history
  MIMO_API_KEY=tp-xxxx ./install_claude_skills_new.sh --skip-skills --skip-cli

  # 连技能集和 CLI 一起装（新电脑首次）
  MIMO_API_KEY=tp-xxxx ./install_claude_skills_new.sh -y ~/openvela/vela_ap

说明:
  只支持 Token Plan 的专属 API Key（tp- 开头），固定走 token-plan-cn 专属 Base URL。
  配置写入 ~/.claude/settings.json（权限 600），包含官方要求的完整模型映射：
  ANTHROPIC_MODEL / ANTHROPIC_DEFAULT_{SONNET,OPUS,HAIKU}_MODEL 全部指向 MiMo 模型。
  只设 ANTHROPIC_MODEL 会导致 Claude Code 的内部小模型调用请求 claude-haiku-* 而报错。
EOF
}

# ============================================================
# 1. 解析参数
# ============================================================
# 取选项参数值，缺参数时给出明确报错（而不是 shift 2 失败后静默退出）
need_arg() {
  [ $# -ge 2 ] && [ -n "${2:-}" ] || die "选项 $1 缺少参数值"
  printf '%s' "$2"
}

while [ $# -gt 0 ]; do
  case "$1" in
    -r|--root)     PROJECT_ROOT="$(need_arg "$@")"; shift 2 ;;
    -k|--api-key)  API_KEY="$(need_arg "$@")";      shift 2 ;;
    -b|--base-url) BASE_URL="$(need_arg "$@")";     shift 2 ;;
    -m|--model)    MODEL="$(need_arg "$@")";        shift 2 ;;
    --1m)          USE_1M=1;              shift ;;
    -y|--yes)      ASSUME_YES=1;          shift ;;
    --skip-skills) DO_SKILLS=0;           shift ;;
    --skip-cli)    DO_CLI=0;              shift ;;
    --skip-config) DO_CONFIG=0;           shift ;;
    --no-test)     DO_TEST=0;             shift ;;
    -h|--help)     usage; exit 0 ;;
    -*)            die "未知选项: $1（用 --help 查看用法）" ;;
    *)
      [ -n "$PROJECT_ROOT" ] && die "项目根目录重复指定: $PROJECT_ROOT / $1"
      PROJECT_ROOT="$1"; shift ;;
  esac
done

PROJECT_ROOT="${PROJECT_ROOT:-$(pwd)}"
[ -d "$PROJECT_ROOT" ] || die "目录不存在: $PROJECT_ROOT"
PROJECT_ROOT="$(cd "$PROJECT_ROOT" && pwd)"

command -v git >/dev/null 2>&1 || die "未找到 git，请先安装：sudo apt install git"

step "环境检查"
info "项目根目录: $PROJECT_ROOT"
info "HOME:        $HOME"
[ "$ASSUME_YES" = "1" ] && info "模式:        非交互 (--yes)"

# 确认这是 openvela 项目根目录（有 build.sh / nuttx/ 之一）
if [ "$DO_SKILLS" = "1" ] \
   && [ ! -e "$PROJECT_ROOT/build.sh" ] && [ ! -d "$PROJECT_ROOT/nuttx" ]; then
  warn "$PROJECT_ROOT 下没看到 build.sh 或 nuttx/，可能不是 openvela 项目根目录。"
  info "技能集 .claude/ 必须与 nuttx/、build.sh 同级才能被 Claude Code 发现。"
  if ! ask "仍要在此安装技能集吗?" n; then
    info "已跳过技能集安装（继续后面的步骤）。"
    DO_SKILLS=0
  fi
fi

# ============================================================
# 2. 安装技能集 (.claude/)
# ============================================================
TARGET="$PROJECT_ROOT/.claude"

# repo 会向上层目录查找 .repo。若 .claude 由 manifest 管理，手工 clone 会让
# repo sync 报 "unsupported checkout state"，先提醒。
if [ "$DO_SKILLS" = "1" ]; then
  _d="$PROJECT_ROOT"
  while [ "$_d" != "/" ]; do
    if [ -d "$_d/.repo" ]; then
      warn "检测到 repo 工作区根目录: $_d"
      info "如果 manifest 里已包含 .claude 项目，手工 clone 会导致 repo sync 报"
      info "  \"Cannot checkout .claude: ... unsupported checkout state\""
      info "可先查: grep -r 'name=\"\\.claude\"' $_d/.repo/manifests/ 2>/dev/null"
      info "若确实由 repo 管理，就别在此手工装，交给 repo sync 拉取。"
      ask "仍要手工安装技能集吗?" n || { info "已跳过技能集安装。"; DO_SKILLS=0; }
      break
    fi
    _d="$(dirname "$_d")"
  done
fi

if [ "$DO_SKILLS" = "0" ]; then
  step "技能集：已按参数跳过"
elif [ -e "$TARGET" ]; then
  step "技能集：$TARGET 已存在"
  if [ -d "$TARGET/.git" ]; then
    cur_branch="$(git -C "$TARGET" branch --show-current 2>/dev/null || echo '?')"
    skill_n="$(ls "$TARGET/skills" 2>/dev/null | wc -l || echo 0)"
    info "当前分支: $cur_branch    技能数量: $skill_n"
    if [ "$cur_branch" != "$BRANCH" ]; then
      warn "分支不是预期的 $BRANCH，可能是内部版或旧版本。不自动改动。"
    elif [ -n "$(git -C "$TARGET" status --porcelain 2>/dev/null)" ]; then
      warn "有本地未提交改动，跳过更新以免冲突。"
    elif ask "拉取最新技能集 (git pull)?" n; then
      git -C "$TARGET" pull --ff-only && ok "技能集已更新。"
    fi
  else
    info "不是 git 仓库（可能是手动拷贝的版本），保持原样不动。"
  fi
  info "如需重装：先备份或删除 $TARGET 再重跑本脚本。"
else
  step "安装技能集"
  info "来源: $REPO_URL (分支 $BRANCH)"
  info "预检远端可达性..."
  if ! git ls-remote --exit-code --heads "$REPO_URL" "$BRANCH" >/dev/null 2>&1; then
    warn "无法访问 $REPO_URL 的 $BRANCH 分支。"
    info "可能原因：网络/代理问题、仓库需要授权、或分支名已变更。"
    ask "仍要尝试 clone 吗?" n || die "已中止。可设置代理后重试，或用 OPENVELA_SKILLS_BRANCH 指定别的分支。"
  fi
  # clone 中断会留下半个目录，用 trap 清掉
  cleanup_partial() { [ -d "$TARGET" ] && rm -rf "$TARGET" && warn "已清理未完成的 $TARGET"; }
  trap cleanup_partial EXIT
  git clone -b "$BRANCH" --single-branch --depth 1 "$REPO_URL" "$TARGET"
  trap - EXIT

  if [ -d "$TARGET/skills/contest-log-collector" ]; then
    ok "找到 contest-log-collector（大赛日志归集技能）。"
  else
    warn "未找到 contest-log-collector，请确认分支 $BRANCH 是否正确。"
  fi
  info "分支: $(git -C "$TARGET" branch --show-current 2>/dev/null || echo '?')    技能数量: $(ls "$TARGET/skills" 2>/dev/null | wc -l)"

  if [ -x "$TARGET/install_dependencies.sh" ]; then
    if ask "运行 .claude/install_dependencies.sh 安装技能依赖?" y; then
      ( cd "$TARGET" && ./install_dependencies.sh ) || warn "依赖安装返回非 0，可稍后手动重试。"
    else
      info "已跳过；之后可手动运行: (cd $TARGET && ./install_dependencies.sh)"
    fi
  fi
fi

# ============================================================
# 3. 安装 Claude Code CLI
# ============================================================
if [ "$DO_CLI" = "0" ]; then
  step "Claude Code CLI：已按参数跳过"
elif command -v claude >/dev/null 2>&1; then
  step "Claude Code CLI：已安装"
  info "路径: $(command -v claude)    版本: $(claude --version 2>/dev/null || echo '?')"
else
  step "安装 Claude Code CLI"
  if ! ask "现在安装 Claude Code CLI (npm install -g @anthropic-ai/claude-code)?" y; then
    info "已跳过。"
  elif ! command -v npm >/dev/null 2>&1; then
    warn "未找到 npm/Node.js，无法安装。"
    info "推荐用 nvm 装 Node 20（免 sudo，避免全局权限问题）："
    info "  curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.1/install.sh | bash"
    info "  exec \$SHELL -l && nvm install 20"
    info "装好 Node 后重跑本脚本（可加 --skip-skills 只补 CLI 和配置）。"
  else
    NODE_MAJOR="$(node -v 2>/dev/null | sed -E 's/^v([0-9]+).*/\1/' || echo 0)"
    proceed=1
    if [ "${NODE_MAJOR:-0}" -lt 18 ]; then
      warn "当前 Node $(node -v 2>/dev/null) < 18，Claude Code 要求 >= 18。"
      ask "仍要尝试安装吗?" n || { info "已跳过 CLI 安装。"; proceed=0; }
    fi
    if [ "$proceed" = "1" ]; then
      info "npm install -g @anthropic-ai/claude-code"
      if npm install -g @anthropic-ai/claude-code; then
        hash -r 2>/dev/null || true
        if command -v claude >/dev/null 2>&1; then
          ok "claude 已安装（$(claude --version 2>/dev/null || echo 已装)）。"
        else
          warn "安装命令成功但 PATH 里找不到 claude，可能需要新开终端。"
          info "npm 全局 bin 目录: $(npm prefix -g 2>/dev/null)/bin"
        fi
      else
        warn "npm 安装失败。"
        info "若是 EACCES 权限错误，不要用 sudo npm，改用 nvm 管理 Node 更省心。"
      fi
    fi
  fi
fi

# ============================================================
# 4. 配置 MiMo 接入
# ============================================================
# JSON 合并写入：优先 node，其次 python3，都没有则备份后直接覆盖
# 参数通过环境变量传递，避免密钥进命令行/被 shell 解释
merge_json() {
  local file="$1" patch="$2" tmp
  mkdir -p "$(dirname "$file")"
  if [ -f "$file" ]; then
    cp -p "$file" "$file.bak.$(date +%Y%m%d%H%M%S)"
    info "已备份原文件: $(basename "$file").bak.*"
  fi
  if command -v node >/dev/null 2>&1; then
    CC_FILE="$file" CC_PATCH="$patch" node -e '
      const fs = require("fs");
      const f = process.env.CC_FILE;
      const patch = JSON.parse(process.env.CC_PATCH);
      let cur = {};
      if (fs.existsSync(f)) {
        const t = fs.readFileSync(f, "utf8").trim();
        if (t) { try { cur = JSON.parse(t); } catch (e) { console.error("    警告: 原文件不是合法 JSON，已按新内容重建（原件已备份）"); cur = {}; } }
      }
      const deep = (a, b) => {
        for (const k of Object.keys(b)) {
          if (b[k] && typeof b[k] === "object" && !Array.isArray(b[k])) {
            a[k] = deep(a[k] && typeof a[k] === "object" && !Array.isArray(a[k]) ? a[k] : {}, b[k]);
          } else { a[k] = b[k]; }
        }
        return a;
      };
      fs.writeFileSync(f, JSON.stringify(deep(cur, patch), null, 2) + "\n");
    '
  elif command -v python3 >/dev/null 2>&1; then
    CC_FILE="$file" CC_PATCH="$patch" python3 -c '
import json, os, sys
f = os.environ["CC_FILE"]; patch = json.loads(os.environ["CC_PATCH"]); cur = {}
if os.path.exists(f):
    t = open(f, encoding="utf-8").read().strip()
    if t:
        try: cur = json.loads(t)
        except Exception:
            sys.stderr.write("    警告: 原文件不是合法 JSON，已按新内容重建（原件已备份）\n"); cur = {}
def deep(a, b):
    for k, v in b.items():
        if isinstance(v, dict): a[k] = deep(a.get(k) if isinstance(a.get(k), dict) else {}, v)
        else: a[k] = v
    return a
open(f, "w", encoding="utf-8").write(json.dumps(deep(cur, patch), indent=2, ensure_ascii=False) + "\n")
'
  else
    warn "没有 node / python3，无法合并 JSON，直接覆盖写入（原件已备份）。"
    printf '%s\n' "$patch" > "$file"
  fi
}

if [ "$DO_CONFIG" = "0" ]; then
  step "MiMo 接入配置：已按参数跳过"
else
  step "配置 Token Plan 专属 API Key"

  # -- 取 key --
  if [ -z "$API_KEY" ]; then
    if [ "$ASSUME_YES" = "1" ]; then
      warn "--yes 模式但未提供专属 API Key，跳过配置。请用 -k 或 MIMO_API_KEY 传入。"
      DO_CONFIG=0
    elif [ ! -t 0 ]; then
      warn "非交互环境且未提供专属 API Key，跳过配置。"
      DO_CONFIG=0
    else
      # 只用一个不回显的提示：既避免 key 打在屏幕上，也避免把 key 粘到 y/n 提示上
      info "粘贴控制台「专属 API key」（tp- 开头）后回车；不想配就直接回车跳过。"
      info "（输入不回显，屏幕上不会显示，这是正常的）"
      for _try in 1 2 3; do
        read -rsp "    专属 API Key (tp-...): " API_KEY || API_KEY=""
        echo
        API_KEY="$(normalize_key "$API_KEY")"
        case "$API_KEY" in
          ""|[yY]|[nN]|[yY][eE][sS]|[nN][oO])
            # 空 = 跳过；y/n = 明显把这里当成了确认提示
            if [ -n "$API_KEY" ]; then
              warn "这里要的是 API Key 本身（tp- 开头），不是 y/n。"
              API_KEY=""
              continue
            fi
            break ;;
          *) break ;;
        esac
      done
      if [ -z "$API_KEY" ]; then
        info "已跳过。之后可重跑: $0 --skip-skills --skip-cli -k <你的key>"
        DO_CONFIG=0
      fi
    fi
  else
    API_KEY="$(normalize_key "$API_KEY")"
  fi
  [ -z "$API_KEY" ] && [ "$DO_CONFIG" = "1" ] && { warn "未输入 key，跳过配置。"; DO_CONFIG=0; }
fi

if [ "$DO_CONFIG" = "1" ]; then
  # -- 校验 key 格式（同时防止奇怪字符破坏 JSON/shell）--
  case "$API_KEY" in
    sk-*)
      die "这是按量付费的 key（sk- 开头），本脚本只用 Token Plan 专属 API Key。
       请到控制台 Token Plan 页面的「专属 API key」栏复制 tp- 开头的那个。"
      ;;
  esac
  if ! printf '%s' "$API_KEY" | grep -qE '^tp-[A-Za-z0-9._~-]+$'; then
    warn "Key 格式不像 Token Plan 的专属 API Key（应为 tp- 开头）。"
    info "位置：控制台 -> Token Plan -> 专属 API key。"
    info "若平台已变更 key 格式，可忽略此提示继续。"
    ask "仍要继续吗?" y || die "已中止，请到控制台复制正确的专属 API Key。"
  fi

  # -- Base URL：固定用 Token Plan 专属地址，除非 -b 显式指定 --
  if [ -z "$BASE_URL" ]; then
    BASE_URL="$BASE_URL_TOKENPLAN"
  elif [ "$BASE_URL" != "$BASE_URL_TOKENPLAN" ]; then
    warn "使用你指定的 Base URL（默认应为 $BASE_URL_TOKENPLAN）。"
  fi
  BASE_URL="${BASE_URL%/}"   # 去掉结尾斜杠，避免拼出 //v1/messages
  case "$BASE_URL" in
    https://*) : ;;
    http://*)  warn "Base URL 用的是 http，密钥会明文传输，建议改 https。" ;;
    *)         die "Base URL 必须以 http(s):// 开头: $BASE_URL" ;;
  esac

  # -- 模型 ID，可选 1M 上下文 --
  MODEL_ID="$MODEL"
  if [ "$USE_1M" = "1" ]; then
    case "$MODEL_ID" in *"[1m]") : ;; *) MODEL_ID="${MODEL_ID}[1m]" ;; esac
  fi

  info "Base URL: $BASE_URL"
  info "API Key:  $(mask_key "$API_KEY")"
  info "模型:     $MODEL_ID"

  # -- 写 ~/.claude/settings.json --
  # 四个模型变量必须都配：Claude Code 内部会按 sonnet/opus/haiku 三档分别发请求，
  # 只设 ANTHROPIC_MODEL 会让后台任务去请求 claude-haiku-*，MiMo 不认这个模型名。
  # ANTHROPIC_SMALL_FAST_MODEL 是给旧版 CLI 的兼容项，多写无害。
  mkdir -p "$CLAUDE_DIR"
  J_URL="$(json_escape "$BASE_URL")"
  J_KEY="$(json_escape "$API_KEY")"
  J_MODEL="$(json_escape "$MODEL_ID")"
  PATCH_SETTINGS="$(printf '{"env":{'
    printf '"ANTHROPIC_BASE_URL":"%s",'             "$J_URL"
    printf '"ANTHROPIC_AUTH_TOKEN":"%s",'           "$J_KEY"
    printf '"ANTHROPIC_MODEL":"%s",'                "$J_MODEL"
    printf '"ANTHROPIC_DEFAULT_SONNET_MODEL":"%s",' "$J_MODEL"
    printf '"ANTHROPIC_DEFAULT_OPUS_MODEL":"%s",'   "$J_MODEL"
    printf '"ANTHROPIC_DEFAULT_HAIKU_MODEL":"%s",'  "$J_MODEL"
    printf '"ANTHROPIC_SMALL_FAST_MODEL":"%s"'      "$J_MODEL"
    printf '}}')"
  merge_json "$SETTINGS_FILE" "$PATCH_SETTINGS"
  chmod 600 "$SETTINGS_FILE"
  ok "已写入 $SETTINGS_FILE (权限 600，含密钥)"

  # -- 写 ~/.claude.json，跳过首次登录引导 --
  merge_json "$CLAUDE_JSON" '{"hasCompletedOnboarding":true}'
  ok "已写入 $CLAUDE_JSON (hasCompletedOnboarding)"

  # -- ~/.claude 可能是技能集 git 仓库，防止把密钥提交上去 --
  if [ -d "$CLAUDE_DIR/.git" ]; then
    warn "$CLAUDE_DIR 是一个 git 仓库，settings.json 含密钥，切勿提交！"
    excl="$CLAUDE_DIR/.git/info/exclude"
    mkdir -p "$(dirname "$excl")"
    if ! { [ -f "$excl" ] && grep -qx "settings.json" "$excl"; }; then
      printf 'settings.json\nsettings.json.bak.*\n' >> "$excl"
      info "已加入 .git/info/exclude，git status 不再显示它。"
    fi
  fi

  # -- 清理会打架的旧配置 --
  conflict=0
  for v in ANTHROPIC_API_KEY ANTHROPIC_AUTH_TOKEN ANTHROPIC_BASE_URL; do
    if [ -n "${!v:-}" ]; then warn "当前 shell 里存在 $v，可能与新配置冲突。"; conflict=1; fi
  done
  if [ -f "$LEGACY_ENV_FILE" ]; then
    warn "发现旧版脚本留下的 $LEGACY_ENV_FILE，它导出的环境变量优先级可能覆盖 settings.json。"
    if ask "禁用它（重命名为 .disabled 并从 ~/.bashrc 摘掉加载行）?" y; then
      mv "$LEGACY_ENV_FILE" "$LEGACY_ENV_FILE.disabled"
      for rc in "$HOME/.bashrc" "$HOME/.zshrc"; do
        if [ -f "$rc" ] && grep -qF ".claude_code_env" "$rc"; then
          cp -p "$rc" "$rc.bak.$(date +%Y%m%d%H%M%S)"
          sed -i 's|^\(.*\.claude_code_env.*\)$|# [disabled by install_claude_skills_new.sh] \1|' "$rc"
          info "已注释 $rc 里的加载行（原文件已备份）。"
        fi
      done
      ok "旧配置已禁用。"
    else
      info "保留旧文件。若行为异常，手动删掉它再试。"
    fi
    conflict=1
  fi

  # -- 扫 shell 启动文件里直接写死的 ANTHROPIC_* export --
  # 这类 export 优先级高于 settings.json，是"配置改了但不生效 / 401"的常见原因
  for rc in "$HOME/.bashrc" "$HOME/.bash_profile" "$HOME/.profile" "$HOME/.zshrc" "$HOME/.zshenv"; do
    [ -f "$rc" ] || continue
    hits="$(grep -nE '^[[:space:]]*(export[[:space:]]+)?ANTHROPIC_[A-Z_]+=' "$rc" 2>/dev/null || true)"
    [ -z "$hits" ] && continue
    warn "$rc 里有写死的 ANTHROPIC_* 变量，会覆盖 settings.json："
    printf '%s\n' "$hits" | sed 's/^/      /'
    if ask "注释掉这些行（原文件会备份）?" y; then
      cp -p "$rc" "$rc.bak.$(date +%Y%m%d%H%M%S)"
      sed -i -E 's|^([[:space:]]*(export[[:space:]]+)?ANTHROPIC_[A-Z_]+=.*)$|# [disabled by install_claude_skills_new.sh] \1|' "$rc"
      ok "已注释并备份 $rc"
    else
      info "保留原样。注意：只要这些 export 还在，settings.json 就会被压着。"
    fi
    conflict=1
  done

  if [ "$conflict" = "1" ]; then
    info "务必新开一个终端再启动 claude（当前 shell 里的旧变量不会自动消失）。"
    info "验证: env | grep -i anthropic  应该没有输出。"
  fi
fi

# ============================================================
# 5. 连通性自检
# ============================================================
if [ "$DO_CONFIG" = "1" ] && [ "$DO_TEST" = "1" ] && command -v curl >/dev/null 2>&1; then
  step "连通性自检"
  info "向 $BASE_URL/v1/messages 发一个最小请求（消耗极少量额度）"
  http_code="$(
    curl -sS -o /tmp/mimo_probe.$$ -w '%{http_code}' \
      --max-time 45 \
      -X POST "$BASE_URL/v1/messages" \
      -H "content-type: application/json" \
      -H "anthropic-version: $ANTHROPIC_API_VERSION" \
      -H "x-api-key: $API_KEY" \
      -d "{\"model\":\"$MODEL_ID\",\"max_tokens\":16,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}" \
      2>/dev/null || echo "000"
  )"
  case "$http_code" in
    200)
      ok "接口连通，模型 $MODEL_ID 可用。"
      ;;
    401|403)
      warn "HTTP $http_code —— 认证失败。到控制台 Token Plan 页确认专属 API Key 是否已重新生成过，或套餐是否过期。"
      ;;
    404)
      warn "HTTP $http_code —— 地址或模型不存在。检查 Base URL 是否为 Anthropic 兼容协议地址，以及模型 ID $MODEL_ID 是否在套餐权益内。"
      ;;
    429)
      warn "HTTP $http_code —— 限流或额度用尽，配置本身应该没问题。"
      ;;
    000)
      warn "请求未送达（网络/DNS/代理问题），配置已写入，联网后再试。"
      ;;
    *)
      warn "HTTP $http_code，配置已写入但接口未返回成功。"
      ;;
  esac
  if [ "$http_code" != "200" ] && [ -s "/tmp/mimo_probe.$$" ]; then
    info "服务端响应片段："
    head -c 400 "/tmp/mimo_probe.$$" | sed 's/^/      /'
    echo
  fi
  rm -f "/tmp/mimo_probe.$$"
fi

# ============================================================
# 6. 完成
# ============================================================
step "完成"
cat <<EOF
后续步骤:
  1. 新开一个终端（让配置和 PATH 生效）
  2. cd "$PROJECT_ROOT" && claude
  3. 首次启动选 "Trust This Folder"
  4. 进去后用 /status 看当前 Base URL 和模型；/context 看上下文窗口

排查:
  - 报模型不存在      -> 检查 $SETTINGS_FILE 里四个 ANTHROPIC_*MODEL 是否都是 MiMo 模型名
  - 认证失败          -> 到控制台 Token Plan 页核对「专属 API key」，确认没被重新生成、套餐没过期
  - 配置似乎没生效    -> env | grep ANTHROPIC 看有没有旧变量覆盖；环境变量优先级高于 settings.json
  - 想开 1M 上下文    -> 重跑本脚本加 --1m，或手动把模型改成 ${MODEL}[1m]

注意:
  - .claude/ 技能集必须与 nuttx/、build.sh 同级（项目根目录）
  - $SETTINGS_FILE 含密钥、权限 600，切勿提交到 git 或分享截图
  - 官方文档: https://mimo.mi.com/docs/zh-CN/tokenplan/integration/claudecode
EOF
