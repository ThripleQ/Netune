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
        # Note: libyyjson-dev is omitted because it is not available in some
        # Debian/Ubuntu releases. CMake will fall back to building yyjson from
        # source automatically.
        sudo apt-get install -y -qq \
            cmake pkg-config \
            libavformat-dev libavcodec-dev libswresample-dev \
            libasound2-dev libpulse-dev libsdl2-dev \
            libyaml-dev
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
    local required_major=1
    local required_minor=22
    local target_version="1.23.4"

    download_and_install_go() {
        echo "==> Installing Go ${target_version} (needed for Netease features)..."

        local arch
        case "$(uname -m)" in
            x86_64)  arch="amd64" ;;
            aarch64) arch="arm64" ;;
            armv7l)  arch="armv6l" ;;
            *)       arch="$(uname -m)" ;;
        esac

        local os
        case "$(uname -s)" in
            Linux)  os="linux" ;;
            Darwin) os="darwin" ;;
            *)      os="$(uname -s | tr '[:upper:]' '[:lower:]')" ;;
        esac

        local tarball="go${target_version}.${os}-${arch}.tar.gz"
        local url="https://go.dev/dl/${tarball}"
        local tmpdir
        tmpdir=$(mktemp -d)
        local install_dir="/usr/local"

        local urls=(
            "https://go.dev/dl/${tarball}"
            "https://mirrors.aliyun.com/golang/${tarball}"
            "https://mirrors.tuna.tsinghua.edu.cn/golang/${tarball}"
        )
        local downloaded=0
        for url in "${urls[@]}"; do
            echo "Trying ${url} ..."
            if command -v wget &>/dev/null; then
                if wget -q -O "${tmpdir}/${tarball}" "${url}"; then
                    downloaded=1
                    break
                fi
            elif command -v curl &>/dev/null; then
                if curl -fsSL --retry 3 -o "${tmpdir}/${tarball}" "${url}"; then
                    downloaded=1
                    break
                fi
            else
                echo "ERROR: curl or wget is required to download Go."
                rm -rf "${tmpdir}"
                return 1
            fi
        done

        if [ "$downloaded" -ne 1 ] || [ ! -s "${tmpdir}/${tarball}" ]; then
            echo "ERROR: Failed to download Go tarball from any mirror."
            rm -rf "${tmpdir}"
            return 1
        fi

         if [ -d "${install_dir}/go" ]; then
            echo "Removing existing ${install_dir}/go ..."
            sudo rm -rf "${install_dir}/go"
        fi

        echo "Extracting Go to ${install_dir} ..."
        sudo tar -C "${install_dir}" -xzf "${tmpdir}/${tarball}"
        sudo ln -sf "${install_dir}/go/bin/go" "${install_dir}/bin/go"

        rm -rf "${tmpdir}"
        echo "Go ${target_version} installed to ${install_dir}/go"
    }

    if ! command -v go &>/dev/null; then
        download_and_install_go
        return
    fi

    local version
    version=$(go version | awk '{print $3}' | sed 's/^go//')
    local major=$(echo "$version" | cut -d. -f1)
    local minor=$(echo "$version" | cut -d. -f2)

    if [ "$major" -lt "$required_major" ] || \
       ([ "$major" -eq "$required_major" ] && [ "$minor" -lt "$required_minor" ]); then
        echo "Go $version is installed, but netease-cli requires Go >= $required_major.$required_minor. Upgrading ..."
        download_and_install_go
    else
        echo "Go $version installed (>= $required_major.$required_minor)."
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
