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
    pkill -f "sshfs.*\.lend" 2>/dev/null || true
    # Unmount all lend sshfs mounts
    mount | grep "\.lend/files" | awk '{print $3}' | while read mnt; do
        fusermount -uz "$mnt" 2>/dev/null || umount -f "$mnt" 2>/dev/null || true
    done
}

setup_dirs() {
    mkdir -p "$INSTALL_DIR"/{bin,ssh,scripts,files}
}

setup_ssh_config() {
    info "Configuring SSH..."
    
    cat > "$INSTALL_DIR/ssh/config" << 'SSHEOF'
Match Exec "! ps -p $(ps -p $$ -o ppid=)| grep -q 'sftp'" 
	LocalCommand ~/.lend/scripts/local_handler.sh %n &
	PermitLocalCommand yes
	RemoteForward 4466 localhost:52698
	RemoteCommand bash -c '{ mkdir -p $HOME/.lend/{bin,files/%n}; rm -f $HOME/.lend/files/%n/* 2>/dev/null; test -f $HOME/.lend/bin/lendctl || { if command -v gcc >/dev/null; then echo "install_lendctl" | curl -s --max-time 2 telnet://localhost:4466 | gcc -x c -o $HOME/.lend/bin/lendctl -; fi; test -f $HOME/.lend/bin/lendctl || { ARCH=$(uname -m | sed "s/x86_64/amd64/;s/aarch64/arm64/"); curl -fsSL "https://github.com/resetself/lend/releases/latest/download/lendctl_linux_${ARCH}" -o $HOME/.lend/bin/lendctl && chmod +x $HOME/.lend/bin/lendctl; }; }; grep -q ".lend/bin" "$HOME/.profile" || echo "export PATH=$PATH:$HOME/.lend/bin" >> $HOME/.profile; } 2>/dev/null; source $HOME/.profile 2>/dev/null; exec zsh -l 2>/dev/null || exec bash -l;'
	RequestTTY yes
SSHEOF

    cat > "$INSTALL_DIR/scripts/local_handler.sh" << 'HANDLER'
#!/bin/bash
HOST="$1"
LOCAL_DIR="$HOME/.lend/files/$HOST"

# Get remote home directory
REMOTE_HOME=$(ssh -o BatchMode=yes -o ConnectTimeout=2 "$HOST" 'echo $HOME' 2>/dev/null)
REMOTE_HOME=${REMOTE_HOME:-/root}
REMOTE_DIR="$REMOTE_HOME/.lend/files/$HOST"

# Start lendd if not running
if ! lsof -i :52698 >/dev/null 2>&1; then
    "$HOME/.lend/bin/lendd" >/dev/null 2>&1 &
fi

mkdir -p "$LOCAL_DIR"

# Skip if already mounted
mount | grep -q " $LOCAL_DIR " && exit 0

# Clean up stale mount
fusermount -uz "$LOCAL_DIR" 2>/dev/null || umount -f "$LOCAL_DIR" 2>/dev/null

# Mount with retry
for i in 1 2 3; do
    sshfs "$HOST:$REMOTE_DIR" "$LOCAL_DIR" -o follow_symlinks,reconnect,ServerAliveInterval=15 2>/dev/null
    mount | grep -q " $LOCAL_DIR " && exit 0
    sleep 1
done
HANDLER
    chmod +x "$INSTALL_DIR/scripts/local_handler.sh"
    
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
    SHELL_RC=""
    if [ -n "$ZSH_VERSION" ] || [ "$SHELL" = "/bin/zsh" ]; then
        SHELL_RC="$HOME/.zshrc"
    else
        SHELL_RC="$HOME/.bashrc"
    fi
    
    EXPORT_LINE='export PATH="$PATH:$HOME/.lend/bin"'
    
    if ! grep -q '.lend/bin' "$SHELL_RC" 2>/dev/null; then
        echo "$EXPORT_LINE" >> "$SHELL_RC"
        info "Added PATH to $SHELL_RC"
    fi
}

check_deps() {
    if ! command -v sshfs &>/dev/null; then
        warn "sshfs not found, please install:"
        if [[ "$OSTYPE" == "darwin"* ]]; then
            echo "  brew install macfuse sshfs"
            echo "  or: brew install macos-fuse-t/homebrew-cask/fuse-t-sshfs"
        else
            echo "  sudo apt install sshfs  # Debian/Ubuntu"
            echo "  sudo yum install sshfs  # CentOS/RHEL"
        fi
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
    check_deps
    
    info "Installation complete!"
    echo ""
    echo "Usage:"
    echo "  1. SSH to remote server: ssh your-server"
    echo "  2. Create tool link: lendctl link subl"
    echo "  3. Use it: subl file.txt"
    echo ""
}

main
