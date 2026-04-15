#!/bin/bash

# 1. Setup Global Variables
REPO="GalaxyHaze/Zith-Lang"
VERSION=""
OUTPUT_NAME="zith"

# 2. Determine Version
if [ -n "$1" ]; then
    VERSION="$1"
    echo "Installing requested version: $VERSION"
else
    # We use the GitHub API. To avoid strict rate limits (60/hr), we avoid auth 
    # but keep the URL standard. If this hits a limit, it will return 403.
    API_URL="https://api.github.com/repos/$REPO/releases/latest"
    
    # Fetch latest tag
    VERSION=$(curl -s "$API_URL" | grep '"tag_name":' | sed -E 's/.*"([^"]+)".*/\1/')
    
    if [ -z "$VERSION" ]; then
        echo "Error: Could not fetch latest version from GitHub (API Rate Limit?)."
        echo "Please specify a version manually: ./install.sh v1.0.0"
        exit 1
    fi
    echo "No version specified. Installing latest version: $VERSION"
fi


# 3. Detect OS and Architecture
OS="$(uname -s)"
ARCH="$(uname -m)"

FILE_NAME=""

# Determine Filename based on OS and Arch
case "$OS" in
  Linux*)  
      case "$ARCH" in
          x86_64) FILE_NAME="zith-linux-amd64" ;;
          aarch64|arm64) FILE_NAME="zith-linux-arm64" ;;
          *) echo "Architecture not supported on Linux: $ARCH"; exit 1 ;;
      esac
      ;;
  Darwin*) 
      case "$ARCH" in
          x86_64) FILE_NAME="zith-macos-amd64" ;;
          arm64) FILE_NAME="zith-macos-arm64" ;;
          *) echo "Architecture not supported on macOS: $ARCH"; exit 1 ;;
      esac
      ;;
  # Covers Git Bash, MinGW, and MSYS on Windows
  MINGW*|MSYS*|CYGWIN*)
      FILE_NAME="zith-windows-amd64.exe"
      OUTPUT_NAME="zith.exe"
      ;;
  *)      echo "OS not supported: $OS"; exit 1 ;;
esac

# Safety Check
if [ -z "$FILE_NAME" ]; then
    echo "Error: Could not find a compatible binary for $OS $ARCH."
    exit 1
fi

DOWNLOAD_URL="https://github.com/$REPO/releases/download/$VERSION/$FILE_NAME"

echo "Downloading $FILE_NAME from $DOWNLOAD_URL..."

# 4. Download the binary
if ! curl -fsSL "$DOWNLOAD_URL" -o "$OUTPUT_NAME"; then
    echo "Error: Failed to download binary."
    echo "Please check the URL: $DOWNLOAD_URL"
    exit 1
fi

# 5. Install
chmod +x "$OUTPUT_NAME"

if [[ "$OS" == MINGW* ]] || [[ "$OS" == MSYS* ]] || [[ "$OS" == CYGWIN* ]]; then
    # Windows (Git Bash) - Just chmod, user must move it manually
    # or add it to PATH manually. 
    echo "Download complete: $OUTPUT_NAME"
    echo "Please move '$OUTPUT_NAME' to a folder in your PATH."
else
    # Linux / macOS
    echo "Installing Zith to /usr/local/bin/..."
    
    # Using 'sudo -v' checks if we have sudo rights nicely
    if sudo mv "$OUTPUT_NAME" /usr/local/bin/zith; then
        echo "Installation complete! Run 'zith --help' to get started."
    else
        echo "Installation failed. Please check your sudo permissions or try manually moving the file."
        exit 1
    fi
fi