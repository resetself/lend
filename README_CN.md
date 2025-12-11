# Lend

> 把本地工具"借"给远程服务器用

SSH 连接远程主机后，无需在远程安装任何软件，直接调用本地工具处理远程文件。

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

**Lend 的方案**：远程服务器"借用"你本地的工具，文件通过 SSHFS 挂载，命令转发到本地执行。

## 工作原理

```
远程服务器                         本地机器
    │                                │
    │  1. subl file.txt              │
    │  ─────────────────────────────>│
    │                                │  2. 本地 sublime 打开
    │                                │     ~/.lend/files/file.txt
    │                                │     (SSHFS 挂载的远程文件)
    │                                │
    │  3. 保存后直接写入远程          │
    │  <─────────────────────────────│
```

## 安装

**前提**：macOS 或 Linux，能 SSH 连接到远程服务器

### 1. 安装 sshfs（macOS）
```bash
brew install macos-fuse-t/homebrew-cask/fuse-t-sshfs
```

### 2. 安装 lend

* 一键安装
```bash
curl -fsSL https://raw.githubusercontent.com/resetself/lend/main/install.sh | bash
```

* 从源码安装

```bash
git clone https://github.com/resetself/lend.git
cd lend && make && make install
```

## 使用

```bash
# 连接远程服务器（会自动挂载文件系统）
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

## 安全性

- 所有通信走 SSH 加密通道
- 远程只能触发命令，无法访问本地其他文件
- 建议使用 SSH 密钥认证

## License

MIT
