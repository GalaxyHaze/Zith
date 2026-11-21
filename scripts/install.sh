#!/bin/bash
set -euo pipefail

REPO="${ZITH_REPOSITORY:-GalaxyHaze/Zith-Lang}"
VERSION=""
USE_MUSL=false
INSTALL_LSP=false
OUTPUT_NAME="zithc"
INSTALL_ROOT="${ZITH_INSTALL_ROOT:-}"
RELEASE_BASE_URL="${ZITH_RELEASE_BASE_URL:-}"

usage() {
    echo "Usage: $0 [--musl] [--lsp] [<version>]"
    echo "  --musl        Download the musl-linked static binary"
    echo "  --lsp         Also install the Zith language server"
    echo "  <version>     Specific version to install (default: latest)"
    exit 1
}

for arg in "$@"; do
    case "$arg" in
        --musl) USE_MUSL=true ;;
        --lsp) INSTALL_LSP=true ;;
        --help|-h) usage ;;
        -*)
            echo "Unknown option: $arg" >&2
            usage
            ;;
        *)
            if [ -n "$VERSION" ]; then
                echo "Only one version may be specified." >&2
                usage
            fi
            VERSION="$arg"
            ;;
    esac
done

normalize_version() {
    # GitHub asset names use the leading "v", but older releases may be stored
    # with a bare version. Keep the canonical release URL form when possible.
    case "$VERSION" in
        v*) ;; # already canonical
        *) VERSION="v$VERSION" ;;
    esac
}

release_base_url() {
    if [ -n "$RELEASE_BASE_URL" ]; then
        printf '%s\n' "${RELEASE_BASE_URL%/}"
    else
        printf 'https://github.com/%s/releases/download/%s\n' "$REPO" "$VERSION"
    fi
}

download_release_asset() {
    local asset_name="$1"
    local destination="$2"

    if [ -n "${GITHUB_TOKEN:-}" ] && [ -z "$RELEASE_BASE_URL" ] &&
        command -v gh >/dev/null 2>&1; then
        GH_TOKEN="$GITHUB_TOKEN" gh release download "$VERSION" \
            --repo "$REPO" \
            --pattern "$asset_name" \
            --dir "$TMP_DIR"
        local downloaded="$TMP_DIR/$asset_name"
        if [ "$downloaded" != "$destination" ]; then
            mv "$downloaded" "$destination"
        fi
        return
    fi

    curl -fsSL "$(release_base_url)/$asset_name" -o "$destination"
}

detect_latest_version() {
    # Try authenticated request first (spares rate limit), fall back to unauthenticated
    API_URL="https://api.github.com/repos/$REPO/releases/latest"
    if [ -n "${GITHUB_TOKEN:-}" ]; then
        VERSION=$(curl --fail --silent --show-error \
            -H "Authorization: token $GITHUB_TOKEN" "$API_URL" |
            grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/')
    else
        VERSION=$(curl --fail --silent --show-error "$API_URL" |
            grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/')
    fi

    if [ -z "$VERSION" ]; then
        echo "Error: Could not fetch latest version from GitHub (API rate limit?)." >&2
        echo "Please specify a version manually: $0 v1.0.0" >&2
        exit 1
    fi
}

if [ -n "$VERSION" ]; then
    echo "Installing requested version: $VERSION"
    normalize_version
else
    echo "No version specified. Fetching latest version..."
    detect_latest_version
    echo "Latest version found: $VERSION"
fi

OS="$(uname -s)"
ARCH="$(uname -m)"
FILE_NAME=""
LSP_FILE_NAME=""
LSP_OUTPUT_NAME="zith-lsp"
RUNTIME_FILE_NAME=""

case "$OS" in
    Linux*)
        LSP_FILE_NAME="zith-lsp-linux-amd64"
        case "$ARCH" in
            aarch64|arm64) LSP_FILE_NAME="zith-lsp-linux-arm64" ;;
        esac
        if [ "$USE_MUSL" = true ]; then
            case "$ARCH" in
                x86_64)   FILE_NAME="zithc-linux-amd64-musl" ;;
                aarch64|arm64) FILE_NAME="zithc-linux-arm64-musl" ;;
                *) echo "Architecture not supported on Linux (musl): $ARCH" >&2; exit 1 ;;
            esac
        else
            case "$ARCH" in
                x86_64)   FILE_NAME="zithc-linux-amd64" ;;
                aarch64|arm64) FILE_NAME="zithc-linux-arm64" ;;
                *) echo "Architecture not supported on Linux: $ARCH" >&2; exit 1 ;;
            esac
        fi
        ;;
    Darwin*)
        case "$ARCH" in
            arm64|aarch64)
                FILE_NAME="zithc-macos-arm64"
                LSP_FILE_NAME="zith-lsp-macos-arm64"
                RUNTIME_FILE_NAME="zith-macos-runtime-arm64.tar.gz"
                ;;
            x86_64|amd64)
                FILE_NAME="zithc-macos-amd64"
                LSP_FILE_NAME="zith-lsp-macos-amd64"
                RUNTIME_FILE_NAME="zith-macos-runtime-amd64.tar.gz"
                ;;
            *) echo "Architecture not supported on macOS: $ARCH" >&2; exit 1 ;;
        esac
        ;;
    MINGW*|MSYS*|CYGWIN*)
        FILE_NAME="zithc-windows-amd64.exe"
        LSP_FILE_NAME="zith-lsp-windows-amd64.exe"
        LSP_OUTPUT_NAME="zith-lsp.exe"
        OUTPUT_NAME="zithc.exe"
        ;;
    *) echo "OS not supported: $OS" >&2; exit 1 ;;
esac

if [[ "$INSTALL_LSP" == true && "$OS" == Linux* && "$USE_MUSL" == true ]]; then
    echo "Error: the language server does not currently have musl release assets." >&2
    exit 1
fi

DOWNLOAD_URL="$(release_base_url)/$FILE_NAME"
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT
TMP_FILE="$TMP_DIR/$OUTPUT_NAME"

echo "Downloading $FILE_NAME..."
if ! download_release_asset "$FILE_NAME" "$TMP_FILE"; then
    echo "Error: Failed to download binary." >&2
    echo "Please check the URL: $DOWNLOAD_URL" >&2
    exit 1
fi

if [ "$INSTALL_LSP" = true ]; then
    echo "Downloading optional language server $LSP_FILE_NAME..."
    if ! download_release_asset "$LSP_FILE_NAME" "$TMP_DIR/$LSP_OUTPUT_NAME"; then
        echo "Error: Failed to download language server asset $LSP_FILE_NAME." >&2
        exit 1
    fi
    chmod +x "$TMP_DIR/$LSP_OUTPUT_NAME"
fi

if [ "$OS" = "Darwin" ]; then
    RUNTIME_ARCHIVE="$TMP_DIR/$RUNTIME_FILE_NAME"
    RUNTIME_STAGE="$TMP_DIR/macos-runtime"
    echo "Downloading shared macOS runtime..."
    if ! download_release_asset "$RUNTIME_FILE_NAME" "$RUNTIME_ARCHIVE"; then
        echo "Error: Failed to download macOS runtime asset $RUNTIME_FILE_NAME." >&2
        exit 1
    fi
    RUNTIME_MEMBERS="$(tar -tzf "$RUNTIME_ARCHIVE")"
    while IFS= read -r member; do
        case "$member" in
            /*|../*|*/../*|*/..|lib/*/*)
                echo "Error: Unsafe path in macOS runtime archive: $member" >&2
                exit 1
                ;;
            lib/|lib/*.dylib|manifest.json) ;;
            *)
                echo "Error: Unexpected path in macOS runtime archive: $member" >&2
                exit 1
                ;;
        esac
    done <<< "$RUNTIME_MEMBERS"
    mkdir -p "$RUNTIME_STAGE"
    tar -xzf "$RUNTIME_ARCHIVE" -C "$RUNTIME_STAGE"
    if [ ! -s "$RUNTIME_STAGE/manifest.json" ] ||
        ! find "$RUNTIME_STAGE/lib" -maxdepth 1 -type f -name '*.dylib' -print -quit |
            grep -q .; then
        echo "Error: macOS runtime archive has no manifest or dylibs." >&2
        exit 1
    fi
fi

chmod +x "$TMP_FILE"

case "$OS" in
    MINGW*|MSYS*|CYGWIN*)
        PREFIX="${INSTALL_ROOT:-${ZITH_PREFIX:-$HOME/.local}}"
        BIN_DIR="$PREFIX/bin"
        STDLIB_DIR="$PREFIX/share/zith/stdlib"
        STDLIB_STAGE="$TMP_DIR/stdlib"
        mkdir -p "$BIN_DIR" "${STDLIB_DIR%/*}" "$STDLIB_STAGE"
        STDLIB_URL="$(release_base_url)/zithc-stdlib-$VERSION.zip"
        echo "Downloading stdlib..."
        if ! curl -fsSL "$STDLIB_URL" -o "$TMP_DIR/zithc-stdlib.zip"; then
            echo "Error: Failed to download standard library." >&2
            exit 1
        fi
        unzip -q "$TMP_DIR/zithc-stdlib.zip" -d "$STDLIB_STAGE"
        test -f "$STDLIB_STAGE/std/io/console.zithm"
        cp "$TMP_FILE" "$BIN_DIR/$OUTPUT_NAME"
        if [ "$INSTALL_LSP" = true ]; then
            cp "$TMP_DIR/$LSP_OUTPUT_NAME" "$BIN_DIR/$LSP_OUTPUT_NAME"
        fi
        rm -rf "$STDLIB_DIR"
        mv "$STDLIB_STAGE" "$STDLIB_DIR"
        echo "Download complete: $BIN_DIR/$OUTPUT_NAME"
        if [ "$INSTALL_LSP" = true ]; then
            echo "Language server installed to $BIN_DIR/$LSP_OUTPUT_NAME"
        fi
        echo "Standard library extracted to $STDLIB_DIR"
        echo "Please add $BIN_DIR to your PATH."
        ;;
    *)
        if [ -n "$INSTALL_ROOT" ]; then
            BIN_DIR="$INSTALL_ROOT/bin"
            STDLIB_DIR="$INSTALL_ROOT/share/zith/stdlib"
            STDLIB_STAGE="$TMP_DIR/stdlib"
            mkdir -p "$BIN_DIR" "${STDLIB_DIR%/*}" "$STDLIB_STAGE"
            STDLIB_URL="$(release_base_url)/zithc-stdlib-$VERSION.tar.gz"
            if ! download_release_asset "zithc-stdlib-$VERSION.tar.gz" \
                "$TMP_DIR/zithc-stdlib.tar.gz"; then
                echo "Error: Failed to download standard library." >&2
                exit 1
            fi
            tar xzf "$TMP_DIR/zithc-stdlib.tar.gz" -C "$STDLIB_STAGE"
            test -f "$STDLIB_STAGE/std/io/console.zithm"
            mv "$TMP_FILE" "$BIN_DIR/zithc"
            if [ "$INSTALL_LSP" = true ]; then
                mv "$TMP_DIR/$LSP_OUTPUT_NAME" "$BIN_DIR/$LSP_OUTPUT_NAME"
            fi
            if [ "$OS" = "Darwin" ]; then
                RUNTIME_DIR="$INSTALL_ROOT/share/zith/lib"
                mkdir -p "$INSTALL_ROOT/share/zith"
                rm -rf "$RUNTIME_DIR"
                mv "$RUNTIME_STAGE/lib" "$RUNTIME_DIR"
                mv "$RUNTIME_STAGE/manifest.json" "$INSTALL_ROOT/share/zith/runtime-manifest.json"
                echo "macOS runtime installed to $RUNTIME_DIR"
            fi
            rm -rf "$STDLIB_DIR"
            mv "$STDLIB_STAGE" "$STDLIB_DIR"
            echo "Standard library installed to $STDLIB_DIR"
            if [ "$INSTALL_LSP" = true ]; then
                echo "Language server installed to $BIN_DIR/$LSP_OUTPUT_NAME"
            fi
            echo "Installation complete: $BIN_DIR/zithc"
        else
            echo "Installing Zith to /usr/local/bin/..."
            STDLIB_URL="$(release_base_url)/zithc-stdlib-$VERSION.tar.gz"
            STDLIB_DIR="/usr/local/share/zith/stdlib"
            STDLIB_STAGE="$TMP_DIR/stdlib"
            mkdir -p "$STDLIB_STAGE"
            echo "Downloading stdlib..."
            if ! download_release_asset "zithc-stdlib-$VERSION.tar.gz" \
                "$TMP_DIR/zithc-stdlib.tar.gz"; then
                echo "Error: Failed to download standard library." >&2
                exit 1
            fi
            tar xzf "$TMP_DIR/zithc-stdlib.tar.gz" -C "$STDLIB_STAGE"
            test -f "$STDLIB_STAGE/std/io/console.zithm"
            if ! sudo mv "$TMP_FILE" /usr/local/bin/zithc; then
                echo "Installation failed. Check sudo permissions or try manually moving the file." >&2
                exit 1
            fi
            if [ "$INSTALL_LSP" = true ]; then
                sudo mv "$TMP_DIR/$LSP_OUTPUT_NAME" "/usr/local/bin/$LSP_OUTPUT_NAME"
            fi
            if [ "$OS" = "Darwin" ]; then
                RUNTIME_DIR="/usr/local/share/zith/lib"
                sudo rm -rf "$RUNTIME_DIR"
                sudo mkdir -p "$RUNTIME_DIR" /usr/local/share/zith
                sudo mv "$RUNTIME_STAGE/lib" "$RUNTIME_DIR"
                sudo mv "$RUNTIME_STAGE/manifest.json" /usr/local/share/zith/runtime-manifest.json
                echo "macOS runtime installed to $RUNTIME_DIR"
            fi
            sudo rm -rf "$STDLIB_DIR"
            sudo mkdir -p "${STDLIB_DIR%/*}"
            sudo mv "$STDLIB_STAGE" "$STDLIB_DIR"
            echo "Standard library installed to $STDLIB_DIR"
            if [ "$INSTALL_LSP" = true ]; then
                echo "Language server installed to /usr/local/bin/$LSP_OUTPUT_NAME"
            fi
            echo "Installation complete! Run 'zithc --help' to get started."
        fi
        ;;
esac
