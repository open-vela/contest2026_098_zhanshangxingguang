#!/usr/bin/env bash
# setup-claude-mimo.sh
# 给已安装的 Claude Code 配好 Xiaomi MiMo Token Plan 接入(写 ~/.claude/settings.json)。
# claude CLI 与技能集你机器上已就绪,这一步只补 MiMo 密钥/模型配置。
#
# 用法(密钥走环境变量,不要写进命令历史):
#   MIMO_API_KEY=tp-你的专属key bash setup-claude-mimo.sh
#
# 可选:换模型 / 开 1M 上下文
#   MIMO_API_KEY=tp-xxx MIMO_MODEL=mimo-v2.5-pro bash setup-claude-mimo.sh
#
# 说明:
# - 只接受 "tp-" 开头的专属 API key(走套餐额度);"sk-" 是按量付费,不用。
# - Base URL 固定 https://token-plan-cn.xiaomimimo.com/anthropic
# - 四个模型变量都写(Claude Code 内部按 sonnet/opus/haiku 分档发请求,只设一个会报模型不存在)。
# - settings.json 权限设 600,含密钥,别提交 git、别截图外发。

set -e

KEY="${MIMO_API_KEY:-}"
MODEL="${MIMO_MODEL:-mimo-v2.5-pro}"
BASE_URL="https://token-plan-cn.xiaomimimo.com/anthropic"

if [ -z "$KEY" ]; then
  echo "[x] 未提供 MIMO_API_KEY。用法:  MIMO_API_KEY=tp-你的key bash $0"
  exit 1
fi
case "$KEY" in
  tp-*) : ;;
  sk-*) echo "[x] 检测到 sk- 开头(按量付费)。请用控制台『专属 API key』栏的 tp- 开头 key。"; exit 1 ;;
  *)    echo "[!] 警告:key 不是 tp- 开头,继续但请确认无误。" ;;
esac

mkdir -p ~/.claude

# 备份现有 settings.json
if [ -f ~/.claude/settings.json ]; then
  cp ~/.claude/settings.json ~/.claude/settings.json.bak.$(date +%s)
  echo "[i] 已备份原 settings.json"
fi

# 用 python 合并写入 env 段(保留其它已有键)
MIMO_API_KEY="$KEY" MIMO_MODEL="$MODEL" MIMO_BASE_URL="$BASE_URL" python3 - <<'PY'
import json, os, pathlib
p = pathlib.Path.home() / ".claude" / "settings.json"
d = {}
if p.exists():
    try:
        d = json.loads(p.read_text())
    except Exception:
        d = {}
env = d.get("env", {})
key = os.environ["MIMO_API_KEY"]; model = os.environ["MIMO_MODEL"]; base = os.environ["MIMO_BASE_URL"]
env.update({
    "ANTHROPIC_BASE_URL": base,
    "ANTHROPIC_AUTH_TOKEN": key,
    "ANTHROPIC_MODEL": model,
    "ANTHROPIC_DEFAULT_SONNET_MODEL": model,
    "ANTHROPIC_DEFAULT_OPUS_MODEL": model,
    "ANTHROPIC_DEFAULT_HAIKU_MODEL": model,
})
d["env"] = env
p.write_text(json.dumps(d, indent=2, ensure_ascii=False))
print("[✓] 已写 ~/.claude/settings.json (env: BASE_URL / AUTH_TOKEN / 4x MODEL)")
PY

chmod 600 ~/.claude/settings.json

# onboarding 标记, 跳过登录引导
python3 - <<'PY'
import json, pathlib
p = pathlib.Path.home() / ".claude.json"
d = {}
if p.exists():
    try: d = json.loads(p.read_text())
    except Exception: d = {}
d["hasCompletedOnboarding"] = True
p.write_text(json.dumps(d, indent=2, ensure_ascii=False))
print("[✓] 已设 ~/.claude.json hasCompletedOnboarding=true")
PY

echo
echo "[完成] 现在新开一个终端(让配置生效),在 openvela 工作区内跑:  claude"
echo "        进去后用  /status  看 Base URL 与模型(应为 $MODEL)。"
echo "[安全] settings.json 含密钥(权限已设 600),切勿提交 git 或截图外发。"
echo "[提醒] 若旧的 ~/.claude_code_env 存在会静默覆盖配置,检查:  env | grep ANTHROPIC"
