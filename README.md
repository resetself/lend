# Lend

> Lend your local tools to remote servers

Use local tools on remote servers via SSH without installing anything remotely. No FUSE, no SSHFS, no kernel extensions.

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

**Lend's solution**: The remote server "borrows" your local tools. Remote files are copied to your machine, the local tool runs on them, and changes are pushed back — the same fetch-edit-push model used by `vim scp://` (netrw) and Emacs TRAMP, but for arbitrary black-box CLI tools.

## How it works

```
Remote Server                      Local Machine
    │                                │
    │  1. subl config.yaml           │
    │  ─────────────────────────────>│  lendd receives the file content
    │                                │  over the SSH reverse tunnel
    │                                │  2. Local Sublime opens a
    │                                │     cached copy of config.yaml
    │                                │
    │  3. Save → content is pushed   │
    │  <─────────────────────────────│     back over the tunnel
    │                                │
```

1. On connection, a persistent `ssh -N -R` reverse tunnel forwards the remote unix socket `~/.lend/bridge.sock` to the local `lendd` daemon.
2. When you run a tool on the remote, `lendctl` classifies each argument (flag, file, directory, or output file), sends file contents / directory trees to `lendd`, which materializes them under `~/.lend/files/` and runs the local tool.
3. After the tool exits, `lendctl` writes modified files and directories back to the remote paths.

There is no SSHFS mount and no FUSE dependency. Directories are transferred as a compact archive stream (files, subdirectories, and symlinks; sockets/fifos/devices are skipped).

## Installation

**Prerequisites**: macOS or Linux with SSH access to remote servers. No `sshfs`, `macFUSE`, or `fuse-t` required.

### 1. Install lend (local machine)

* One-line install:
```bash
curl -fsSL https://raw.githubusercontent.com/resetself/lend/main/install.sh | bash
```

* Build from source:
```bash
git clone https://github.com/resetself/lend.git
cd lend && make && make install
```

### 2. (Optional) Create a release

On first login the remote fetches the `lendctl.c` source over the tunnel and compiles it with its local `gcc` (zero-install, no network). If `gcc` is unavailable, it falls back to downloading a precompiled `lendctl` from GitHub releases — publish one only if you want that fallback:

```bash
git tag v0.1.0 && git push origin v0.1.0
```

## Usage

```bash
# SSH to remote server (sets up the reverse tunnel and installs lendctl)
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

### GUI editors

GUI editors that fork and return immediately (such as `subl file.txt` or `code file.txt`) exit before you finish editing, so `lendd` would push back unchanged content. Use their wait flags so the process stays alive until the window closes:

```bash
subl -w file.txt
code -w file.txt
```

## How arguments are classified

`lendctl` maps each argument to a type before sending it over the tunnel:

| Argument | Type |
|----------|------|
| Starts with `-` | String (flag) |
| Existing file | File (content sent, written back after) |
| Existing directory | Directory (tree sent, written back after) |
| Non-existent path containing `/` or `.` | Output file (created remotely if the tool writes it) |
| Other non-existent argument | String |

## Security

- All communication travels over the SSH encrypted channel
- The reverse tunnel is a Unix socket restricted to the remote user's `~/.lend` (mode `0700`)
- The local daemon listens only on a Unix socket `~/.lend/lendd.sock` (not a TCP port)
- The remote can only trigger the tools you expose, and only receives the file paths you pass
- SSH key authentication is recommended

## Limitations

- Dash-less compound flags (`tar czf ...`, `7z a ...`) may be misclassified as output files; quote or reorder them when in doubt.
- The remote OpenSSH server must support Unix-domain-socket forwarding (`StreamLocalForwarding`), available in OpenSSH 6.7+.
- Directory write-back overwrites or creates files but never deletes remote files (no diffing).

## License

MIT
