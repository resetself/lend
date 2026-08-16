# Lend

> 把本地工具"借"给远程服务器用

SSH 连接远程主机后，无需在远程安装任何软件，直接调用本地工具处理远程文件。不需要 FUSE、SSHFS，也不需要内核扩展。

```bash
# 在远程服务器上执行，实际由本地 sublime 打开
subl config.yaml

# 用本地 prettier 格式化远程代码
prettier --write app.js

# 用本地 ffmpeg 转换远程视频
ffmpeg -i video.mp4 video.webm
```

[English](README.md)

## 解决什么问题？

- 远程服务器没装你需要的工具（编辑器、格式化、压缩等）
- 不想在每台服务器上重复安装配置
- 没有 root 权限，装不了软件
- 临时机器，装了也白装

**Lend 的方案**：远程服务器"借用"你本地的工具。远程文件先复制到本地，本地工具处理完后再把改动推回远程——与 `vim scp://`（netrw）和 Emacs TRAMP 相同的"拉取-编辑-回传"模型，但面向任意黑盒命令行工具。

## 工作原理

```
远程服务器                         本地机器
    │                                │
    │  1. subl config.yaml           │
    │  ─────────────────────────────>│  lendd 通过 SSH 反向隧道
    │                                │  收到文件内容
    │                                │  2. 本地 sublime 打开
    │                                │     缓存副本 config.yaml
    │                                │
    │  3. 保存后内容经隧道回传       │
    │  <─────────────────────────────│
    │                                │
```

1. 连接时，一个持久的 `ssh -N -R` 反向隧道把远程的 unix socket `~/.lend/bridge.sock` 转发到本地 `lendd` 守护进程。
2. 在远程运行工具时，`lendctl` 会对每个参数分类（选项、文件、目录、输出文件），把文件内容/目录树发给 `lendd`，`lendd` 将其展开到 `~/.lend/files/` 下并执行本地工具。
3. 工具退出后，`lendctl` 把修改过的文件和目录写回远程路径。

不再依赖 SSHFS 挂载和 FUSE。目录以紧凑的归档流传输（文件、子目录、符号链接；跳过 socket/fifo/设备）。

## 安装

**前提**：macOS 或 Linux，能 SSH 连接到远程服务器。无需安装 `sshfs`、`macFUSE`、`fuse-t`。

### 1. 安装 lend（本地机器）

* 一键安装（预编译二进制）
```bash
curl -fsSL https://raw.githubusercontent.com/resetself/lend/main/install.sh | bash
```

* 一键从源码编译安装（需要 Go，无需预编译 release）
```bash
curl -fsSL https://raw.githubusercontent.com/resetself/lend/main/install.sh | bash -s -- --build
```

* 从本地检出目录编译安装
```bash
git clone https://github.com/resetself/lend.git
cd lend && make && make install
```

### 2. （可选）发布一个 release

远程端首次登录时会通过隧道获取 `lendctl.c` 源码并用本地 `gcc` 编译（零安装、无需联网）。若远程没有 `gcc`，则回退到从 GitHub releases 下载预编译二进制——只有需要这个回退时才需要发布版本：

```bash
git tag v0.1.0 && git push origin v0.1.0
```

## 使用

```bash
# 连接远程服务器（自动建立反向隧道并安装 lendctl）
ssh remote-server

# 创建工具链接（只需一次）
lendctl link subl
lendctl link prettier
lendctl link code

# 之后就能直接用了
subl ~/.bashrc
prettier --write *.js
```

## 支持的工具

任何本地命令行工具都可以，常见场景：

| 类型 | 工具示例 |
|------|----------|
| 编辑器 | `subl`, `code`, `vim` |
| 格式化 | `prettier`, `black`, `gofmt` |
| 媒体处理 | `ffmpeg`, `imagemagick` |
| 压缩 | `7z`, `tar`, `zip` |
| 开发工具 | `eslint`, `rubocop`, `shellcheck` |

### GUI 编辑器

像 `subl`、`code` 这类"启动后立即返回"的 GUI 编辑器，会在你编辑完成前就退出，导致 `lendd` 回传的还是未修改的内容，并在编辑器仍打开时就把临时目录删掉。为避免这种情况，`lendd` 会自动追加编辑器的等待选项，让命令一直保持到窗口关闭：

```bash
subl file.txt      # 实际执行 subl -w file.txt
code file.txt      # 实际执行 code -w file.txt
code project/      # 实际执行 code -w project/
```

已自动识别 `subl`/`sublime`/`sublime_text`、`code`/`code-insiders`、`atom`、`gedit` 和 `kate`。其它编辑器请手动传入等待选项（如 `some-editor --wait file.txt`）。`vim` 等终端编辑器需要 TTY，不适合后台守护进程模式。

打开目录时，会先把整棵目录树（含隐藏文件、嵌套子目录和符号链接）物化到本地，因此编辑器能读到目录下所有文件。

## 参数分类规则

`lendctl` 发送前会把每个参数映射为一种类型：

| 参数 | 类型 |
|------|------|
| 以 `-` 开头 | 字符串（选项） |
| 已存在的文件 | 文件（发送内容，执行后回写） |
| 已存在的目录 | 目录（发送树，执行后回写） |
| 不存在的、含 `/` 或 `.` 的路径 | 输出文件（工具写出后回传创建） |
| 其他不存在的参数 | 字符串 |

## 安全性

- 所有通信走 SSH 加密通道
- 反向隧道是 unix socket，仅限远程用户自己的 `~/.lend`（权限 `0700`）
- 本地守护进程只监听 unix socket `~/.lend/lendd.sock`（非 TCP 端口）
- 远程只能触发你暴露的工具，且只能拿到你传入的文件路径
- 建议使用 SSH 密钥认证

## 已知限制

- 无横杠的复合参数（如 `tar czf ...`、`7z a ...`）可能被误判为输出文件，必要时加引号或调整顺序。
- 远程 OpenSSH 服务端需支持 unix socket 转发（`StreamLocalForwarding`，OpenSSH 6.7+ 可用）。
- 目录回写采用"覆盖或新建"策略，不会删除远程多余文件（不做 diff）。

## License

MIT
