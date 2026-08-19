# openvela 比赛代码下载与安装（家用电脑）

本文档说明如何在自己的电脑上下载并搭建 `robot_competition` 这份 openvela 代码，
参数与公司工作机上的完全一致（manifest = gitcode，分支 trunk，manifest 文件 tags/trunk-5.4.xml，开启 Git LFS）。

> 环境要求：**Ubuntu 22.04**（x86_64 / arm64）。不支持 WSL / Docker 容器内编译。
> 磁盘 ≥ 40 GB，内存 ≥ 16 GB。

---

## 一、安装依赖

```bash
sudo apt update
sudo apt install -y git curl python3 build-essential

# Git LFS（本项目含大二进制文件，必须装，否则拉下来是损坏的指针文件）
curl -s https://packagecloud.io/install/repositories/github/git-lfs/script.deb.sh | sudo bash
sudo apt-get install -y git-lfs
git lfs install
```

配置 git 身份（若没配过）：
```bash
git config --global user.name  "你的名字"
git config --global user.email "你的邮箱"
```

---

## 二、安装 repo 工具

```bash
mkdir -p ~/bin
curl -sSL https://storage.googleapis.com/git-repo-downloads/repo > ~/bin/repo
chmod a+x ~/bin/repo
# 加入 PATH（若 ~/bin 尚不在 PATH）
echo 'export PATH="$HOME/bin:$PATH"' >> ~/.bashrc
export PATH="$HOME/bin:$PATH"
repo --version    # 验证
```

> 若访问 googleapis 受限，repo 引导脚本可用清华镜像（见下一步 --repo-url）。

---

## 三、下载源码（repo init + sync）

```bash
# 建一个工作目录（名字随意）
mkdir -p ~/openvela/robot_competition
cd ~/openvela/robot_competition

# 初始化（与工作机一致：gitcode 源 / trunk 分支 / tags/trunk-5.4.xml / 开 LFS）
# --repo-url 用清华镜像拉 repo 自身，避免 googlesource 被墙
repo init \
  -u https://gitcode.com/open-vela/manifests.git \
  -b trunk \
  -m tags/trunk-5.4.xml \
  --repo-url=https://mirrors.tuna.tsinghua.edu.cn/git/git-repo/ \
  --git-lfs

# 同步全部仓库（232 个，首次较慢；-j4 是并发数，网络好可加大到 -j8）
repo sync -c -j4 --no-clone-bundle --force-sync
```

说明：
- `-c`：只同步当前分支，省时间/空间。
- `--no-clone-bundle`：跳过 clone bundle（某些镜像下更稳）。
- `--force-sync`：强制覆盖本地不一致文件（首次同步或中断重来时用）。
- 中途断了直接**重复** `repo sync ...` 即可增量续传。

### 备选源（gitcode 慢或不可用时）
把 `-u` 换成任一：
- GitHub： `-u https://github.com/open-vela/manifests.git`（去掉 --repo-url）
- Gitee：  `-u https://gitee.com/open-vela/manifests.git`（保留 --repo-url 清华镜像）

---

## 四、验证下载完整

```bash
# 项目数应为约 232
ls -d */ | wc -l
# 关键目录在
ls build.sh nuttx apps vendor
# LFS 文件已实体化（不是几十字节的指针）——抽查一个大文件大小是否正常
find . -name "*.img" -size +1M | head
```

---

## 五、编译 & 跑模拟器（可选，验证环境）

```bash
cd ~/openvela/robot_competition
# 模拟器目标示例
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake -j$(nproc)
./emulator.sh cmake_out/vela_goldfish-arm64-v8a-ap/
```

首次编译会自动提示缺哪些 apt 包，按提示 `sudo apt install` 补齐即可。

---

## 六、装比赛的 Claude 技能集

代码就位后，在**项目根目录**装 AI 技能集（用另一份脚本 `install_claude_skills.sh`）：

```bash
~/install_claude_skills.sh ~/openvela/robot_competition
```

它会把 `github.com/open-vela/.claude`（分支 `dev-ai-contest-2026`）克隆到项目根的 `.claude/`，
并可选安装 Claude Code CLI、配置 API Key。

---

## 附：本机实际用过的完整命令（供比对）

```
manifest: https://gitcode.com/open-vela/manifests.git   分支 trunk   -m tags/trunk-5.4.xml   --git-lfs
sync:     repo sync -c -j4 --no-clone-bundle --force-sync
repo 版本: 2.54    项目数: 232
```
