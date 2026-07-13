#!/bin/bash
set -e

echo "==> Netune Installer"
echo ""

# Detect OS
OS="$(uname -s)"
PKG_MGR=""

install_deps_linux() {
    if command -v apt-get &>/dev/null; then
        echo "Detected: Debian/Ubuntu"
        sudo apt-get update -qq
        sudo apt-get install -y -qq \
            cmake pkg-config \
            libavformat-dev libavcodec-dev libswresample-dev \
            libasound2-dev libpulse-dev libsdl2-dev \
            libyyjson-dev libyaml-dev
    elif command -v pacman &>/dev/null; then
        echo "Detected: Arch Linux"
        sudo pacman -S --needed --noconfirm \
            cmake pkg-config \
            ffmpeg alsa-lib libpulse sdl2 \
            yyjson libyaml
    elif command -v dnf &>/dev/null; then
        echo "Detected: Fedora"
        sudo dnf install -y cmake pkgconfig \
            ffmpeg-devel alsa-lib-devel pulseaudio-libs-devel SDL2-devel \
            yyjson-devel libyaml-devel
    elif command -v zypper &>/dev/null; then
        echo "Detected: openSUSE"
        sudo zypper install -y cmake pkg-config \
            ffmpeg-7-libs-devel alsa-devel libpulse-devel libSDL2-devel \
            yyjson-devel libyaml-devel
    else
        echo "Unknown package manager. Install dependencies manually:"
        echo "  cmake ffmpeg alsa pulseaudio sdl2 yyjson libyaml"
        exit 1
    fi
}

install_go() {
    if ! command -v go &>/dev/null; then
        echo "==> Installing Go (needed for Netease features)..."
        if command -v pacman &>/dev/null; then
            sudo pacman -S --noconfirm go
        elif command -v apt-get &>/dev/null; then
            sudo apt-get install -y -qq golang
        else
            echo "Install Go manually: https://go.dev/dl/"
        fi
    else
        echo "Go already installed."
    fi
}

# ── Linux ────────────────────────────────────────────
if [ "$OS" = "Linux" ]; then
    install_deps_linux
    install_go
fi

# ── macOS ────────────────────────────────────────────
if [ "$OS" = "Darwin" ]; then
    if ! command -v brew &>/dev/null; then
        echo "Install Homebrew first: https://brew.sh/"
        exit 1
    fi
    brew install cmake pkg-config ffmpeg sdl2 yyjson libyaml go
fi

# ── Clone or update ──────────────────────────────────
REPO="${REPO:-https://github.com/ThripleQ/Netune.git}"
DIR="${DIR:-$HOME/Projects/netune}"

if [ -d "$DIR" ]; then
    echo "==> Updating existing clone in $DIR"
    cd "$DIR"
    git pull
else
    echo "==> Cloning into $DIR"
    git clone "$REPO" "$DIR"
    cd "$DIR"
fi

# ── Build ────────────────────────────────────────────
echo "==> Building..."
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# ── Build netease-cli (if Go available) ──────────────
if command -v go &>/dev/null; then
    echo "==> Building netease-cli..."
    cd src/plugins/music_sources/netease/netease-cli
    go build -o netease-cli .
    mv netease-cli "$DIR/build/"
    cd "$DIR"
fi

# ── Install ──────────────────────────────────────────
echo "==> Installing to ~/.local/bin/..."
mkdir -p ~/.local/bin
cp build/netune ~/.local/bin/
[ -f build/netease-cli ] && cp build/netease-cli ~/.local/bin/
cp -r data ~/.local/bin/data/

echo ""
echo "Done! Run: netune"
