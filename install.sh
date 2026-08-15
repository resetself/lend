#!/bin/bash
set -e

REPO="resetself/lend"
INSTALL_DIR="$HOME/.lend"
BIN_DIR="$INSTALL_DIR/bin"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

LOCAL_MODE=0
if [ "$1" = "--local" ]; then
    LOCAL_MODE=1
fi

detect_platform() {
    OS=$(uname -s | tr '[:upper:]' '[:lower:]')
    ARCH=$(uname -m)

    case "$ARCH" in
        x86_64) ARCH="amd64" ;;
        aarch64|arm64) ARCH="arm64" ;;
        *) error "Unsupported architecture: $ARCH" ;;
    esac

    case "$OS" in
        darwin|linux) ;;
        *) error "Unsupported OS: $OS" ;;
    esac

    PLATFORM="${OS}_${ARCH}"
    info "Detected platform: $PLATFORM"
}

download_binary() {
    if [ "$LOCAL_MODE" = "1" ]; then
        info "Using local build"
        return
    fi

    info "Fetching latest version..."
    LATEST=$(curl -fsSL "https://api.github.com/repos/$REPO/releases" | grep '"tag_name"' | head -1 | cut -d'"' -f4)

    if [ -z "$LATEST" ]; then
        error "Failed to get latest version, check your network"
    fi

    info "Downloading lend $LATEST..."
    DOWNLOAD_URL="https://github.com/$REPO/releases/download/$LATEST/lendd_${PLATFORM}"

    mkdir -p "$BIN_DIR"
    curl -fsSL "$DOWNLOAD_URL" -o "$BIN_DIR/lendd"
    chmod +x "$BIN_DIR/lendd"

    info "Installed to $BIN_DIR/lendd"
}

cleanup() {
    info "Stopping existing processes..."
    pkill -f lendd 2>/dev/null || true

    # Kill any lingering reverse-forward ssh processes via their pid files
    for pidfile in "$INSTALL_DIR"/forward/*.pid; do
        [ -f "$pidfile" ] || continue
        PID=$(cat "$pidfile" 2>/dev/null)
        [ -n "$PID" ] && kill "$PID" 2>/dev/null || true
        rm -f "$pidfile"
    done
}

setup_dirs() {
    mkdir -p "$INSTALL_DIR"/{bin,ssh,scripts,files,forward}
}

setup_ssh_config() {
    info "Configuring SSH..."

    cat > "$INSTALL_DIR/ssh/config" << 'SSHEOF'
Match !originalhost orb Exec "! ps -p $(ps -p $(sh -c 'echo $PPID') -o ppid=) | grep -q 'sftp'"
	LocalCommand ~/.lend/scripts/ensure_forward.sh %n &
	PermitLocalCommand yes
	RemoteCommand bash -c '{ mkdir -p $HOME/.lend/bin; test -f $HOME/.lend/bin/lendctl || { ARCH=$(uname -m | sed "s/x86_64/amd64/;s/aarch64/arm64/"); curl -fsSL "https://github.com/resetself/lend/releases/latest/download/lendctl_linux_${ARCH}" -o $HOME/.lend/bin/lendctl && chmod +x $HOME/.lend/bin/lendctl; }; grep -q ".lend/bin" "$HOME/.profile" 2>/dev/null || echo "export PATH=$PATH:$HOME/.lend/bin" >> $HOME/.profile; i=0; while [ $i -lt 15 ] && [ ! -S $HOME/.lend/bridge.sock ]; do sleep 0.2; i=$((i+1)); done; } 2>/dev/null; source $HOME/.profile 2>/dev/null; exec $(getent passwd $USER|cut -d: -f7) -l;'
	RequestTTY yes
	SetEnv TERM=xterm-256color
SSHEOF

    cat > "$INSTALL_DIR/scripts/ensure_forward.sh" << 'HANDLER'
#!/bin/bash
HOST="$1"
[ -n "$HOST" ] || exit 0

FORWARD_DIR="$HOME/.lend/forward"
mkdir -p "$FORWARD_DIR"
PIDFILE="$FORWARD_DIR/$HOST.pid"
LOCK="$FORWARD_DIR/$HOST.lock"

# Fast path: a forward for this host is already alive.
if [ -f "$PIDFILE" ]; then
    PID=$(cat "$PIDFILE" 2>/dev/null)
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        exit 0
    fi
    rm -f "$PIDFILE"
fi

# Serialize concurrent startup attempts from parallel ssh sessions.
if ! mkdir "$LOCK" 2>/dev/null; then
    exit 0
fi
trap 'rmdir "$LOCK" 2>/dev/null' EXIT

# Re-check after acquiring the lock.
if [ -f "$PIDFILE" ]; then
    PID=$(cat "$PIDFILE" 2>/dev/null)
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        exit 0
    fi
fi

# Make sure the local daemon is running.
if ! [ -S "$HOME/.lend/lendd.sock" ]; then
    "$HOME/.lend/bin/lendd" >/dev/null 2>&1 &
fi

# Resolve the remote home. Disable RemoteCommand (would hijack `echo $HOME`)
# and PermitLocalCommand (would recurse into this script).
REMOTE_HOME=$(ssh -o PermitLocalCommand=no -o RemoteCommand=none -o BatchMode=yes -o ConnectTimeout=5 "$HOST" 'echo $HOME' 2>/dev/null)
REMOTE_HOME=${REMOTE_HOME:-/root}

# Prepare the remote socket directory and drop any stale socket.
ssh -o PermitLocalCommand=no -o RemoteCommand=none -o BatchMode=yes -o ConnectTimeout=5 "$HOST" \
    "mkdir -p '$REMOTE_HOME/.lend' && chmod 700 '$REMOTE_HOME/.lend' && rm -f '$REMOTE_HOME/.lend/bridge.sock'" 2>/dev/null

# Start a persistent reverse forward, decoupled from the interactive session.
nohup ssh -N \
    -o PermitLocalCommand=no \
    -o RemoteCommand=none \
    -o ExitOnForwardFailure=yes \
    -o StreamLocalBindUnlink=yes \
    -o ServerAliveInterval=30 \
    -o ServerAliveCountMax=3 \
    -o ControlMaster=no \
    -o ControlPersist=no \
    -R "$REMOTE_HOME/.lend/bridge.sock:$HOME/.lend/lendd.sock" \
    "$HOST" >/dev/null 2>&1 &

echo $! > "$PIDFILE"
HANDLER
    chmod +x "$INSTALL_DIR/scripts/ensure_forward.sh"

    SSH_CONFIG="$HOME/.ssh/config"
    INCLUDE_LINE="Include ~/.lend/ssh/config"

    if [ ! -f "$SSH_CONFIG" ]; then
        mkdir -p "$HOME/.ssh"
        printf "\n%s\n" "$INCLUDE_LINE" > "$SSH_CONFIG"
    elif ! grep -q "$INCLUDE_LINE" "$SSH_CONFIG"; then
        awk -v line="$INCLUDE_LINE" 'NR==1{found=0} /^$/ && !found {print; print line; found=1; next} 1' "$SSH_CONFIG" > "$SSH_CONFIG.tmp"
        mv "$SSH_CONFIG.tmp" "$SSH_CONFIG"
    fi
}

setup_path() {
    CURRENT_SHELL=$(basename "$SHELL")

    case "$CURRENT_SHELL" in
        bash)
            RC_FILE="$HOME/.bashrc"
            EXPORT_LINE='export PATH="$PATH:$HOME/.lend/bin"'
            CHECK_PATTERN='\.lend/bin'
            ;;
        zsh)
            RC_FILE="$HOME/.zshrc"
            EXPORT_LINE='export PATH="$PATH:$HOME/.lend/bin"'
            CHECK_PATTERN='\.lend/bin'
            ;;
        fish)
            RC_FILE="$HOME/.config/fish/config.fish"
            EXPORT_LINE='set -gx PATH $PATH $HOME/.lend/bin'
            CHECK_PATTERN='\.lend/bin'
            ;;
        ksh)
            RC_FILE="$HOME/.kshrc"
            EXPORT_LINE='export PATH="$PATH:$HOME/.lend/bin"'
            CHECK_PATTERN='\.lend/bin'
            ;;
        tcsh|csh)
            RC_FILE="$HOME/.tcshrc"
            EXPORT_LINE='setenv PATH ${PATH}:$HOME/.lend/bin'
            CHECK_PATTERN='\.lend/bin'
            ;;
        sh)
            RC_FILE="$HOME/.profile"
            EXPORT_LINE='export PATH="$PATH:$HOME/.lend/bin"'
            CHECK_PATTERN='\.lend/bin'
            ;;
        *)
            echo "Unknown shell: $CURRENT_SHELL, skipping PATH addition" >&2
            exit 1
            ;;
    esac

    mkdir -p "$(dirname "$RC_FILE")" 2>/dev/null

    if ! grep -q "$CHECK_PATTERN" "$RC_FILE" 2>/dev/null; then
        echo "$EXPORT_LINE" >> "$RC_FILE"
        echo "Added PATH to $RC_FILE"
    else
        echo "PATH already configured in $RC_FILE"
    fi
}

main() {
    echo ""
    echo "  Lend - Lend your local tools to remote servers"
    echo ""

    cleanup
    detect_platform
    setup_dirs
    download_binary
    setup_ssh_config
    setup_path

    info "Installation complete!"
    echo ""
    echo "Usage:"
    echo "  1. SSH to remote server: ssh your-server"
    echo "  2. Create tool link: lendctl link subl"
    echo "  3. Use it: subl file.txt"
    echo ""
}

main
