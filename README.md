# Lend

> Lend your local tools to remote servers

Use local tools on remote servers via SSH without installing anything remotely.

```bash
# Run on remote server, opens with local Sublime
subl config.yaml

# Format remote code with local prettier
prettier --write app.js

# Convert remote video with local ffmpeg
ffmpeg -i video.mp4 video.webm
```

[中文文档](README_CN.md)

## Why?

- Remote server doesn't have the tools you need (editors, formatters, etc.)
- Don't want to install and configure on every server
- No root access to install software
- Temporary machines, not worth setting up

**Lend's solution**: Remote server "borrows" your local tools. Files are mounted via SSHFS, commands are forwarded to local execution.

## How it works

```
Remote Server                      Local Machine
    │                                │
    │  1. subl file.txt              │
    │  ─────────────────────────────>│
    │                                │  2. Local Sublime opens
    │                                │     ~/.lend/files/file.txt
    │                                │     (SSHFS mounted remote file)
    │                                │
    │  3. Save writes to remote      │
    │  <─────────────────────────────│
```

## Installation

**Prerequisites**: macOS or Linux, SSH access to remote servers

### 1. Install sshfs (macOS)

```bash
brew install macos-fuse-t/homebrew-cask/fuse-t-sshfs
```

### 2. Install lendctl

* Onlin install lendctl
```bash
curl -fsSL https://raw.githubusercontent.com/resetself/lend/main/install.sh | bash
```

* build from source:

```bash
git clone https://github.com/resetself/lend.git
cd lend && make && make install
```

## Usage

```bash
# SSH to remote server (auto-mounts filesystem)
ssh remote-server

# Create tool links (one-time setup)
lendctl link subl
lendctl link prettier
lendctl link code

# Use them directly
subl ~/.bashrc
prettier --write *.js
```

## Supported Tools

Any local CLI tool works. Common examples:

| Type | Examples |
|------|----------|
| Editors | `subl`, `code`, `vim` |
| Formatters | `prettier`, `black`, `gofmt` |
| Media | `ffmpeg`, `imagemagick` |
| Compression | `7z`, `tar`, `zip` |
| Dev Tools | `eslint`, `rubocop`, `shellcheck` |

## Security

- All communication over SSH encrypted channel
- Remote can only trigger commands, no access to other local files
- SSH key authentication recommended

## License

MIT
