#!/usr/bin/env bash
# ------------------------------------------------------------------
# setup.sh — Setup, build and run script for Butterscotch-C (Linux)
# ------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"

# -- Defaults --
SKIP_DEPS=0
CLEAN=0
NO_BUILD=0
NO_RUN=0
GAME_FILE=""

# -- Colors --
cyan()   { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
green()  { printf '    \033[0;32m%s\033[0m\n' "$*"; }
yellow() { printf '    \033[0;33m%s\033[0m\n' "$*"; }
red()    { printf '\033[0;31mERROR: %s\033[0m\n' "$*"; }

# -- Usage --
usage() {
    cat <<EOF
Usage: ./setup.sh [OPTIONS]

Options:
  --skip-deps     Skip dependency installation
  --clean         Remove build directory before configuring
  --no-build      Skip the build step (just run)
  --no-run        Skip running after build
  --game <path>   Path to game data file
  -h, --help      Show this help
EOF
    exit 0
}

# -- Parse args --
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-deps) SKIP_DEPS=1 ;;
        --clean)     CLEAN=1 ;;
        --no-build)  NO_BUILD=1 ;;
        --no-run)    NO_RUN=1 ;;
        --game)      GAME_FILE="$2"; shift ;;
        -h|--help)   usage ;;
        *) red "Unknown option: $1"; usage ;;
    esac
    shift
done

# ------------------------------------------------------------------
# 1. Install dependencies (Debian/Ubuntu)
# ------------------------------------------------------------------
if [[ $SKIP_DEPS -eq 0 ]]; then
    cyan "Checking dependencies..."

    PACKAGES=(build-essential cmake ninja-build libsdl2-dev libsdl2-image-dev libsdl2-mixer-dev)
    MISSING=()

    for pkg in "${PACKAGES[@]}"; do
        if dpkg -s "$pkg" &>/dev/null; then
            green "$pkg installed"
        else
            MISSING+=("$pkg")
        fi
    done

    if [[ ${#MISSING[@]} -gt 0 ]]; then
        yellow "Installing missing packages: ${MISSING[*]}"
        sudo apt-get update -qq
        sudo apt-get install -y "${MISSING[@]}"
    fi

    green "All dependencies satisfied."
fi

# ------------------------------------------------------------------
# 2. Configure and build
# ------------------------------------------------------------------
if [[ $NO_BUILD -eq 0 ]]; then
    if [[ $CLEAN -eq 1 && -d "$BUILD_DIR" ]]; then
        cyan "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
    fi

    cyan "Configuring with CMake..."
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja

    cyan "Building..."
    cmake --build "$BUILD_DIR"

    echo ""
    echo "================================================"
    green "Build complete!"
    echo "================================================"
fi

# ------------------------------------------------------------------
# 3. Run the game
# ------------------------------------------------------------------
if [[ $NO_RUN -eq 0 ]]; then
    EXE="$BUILD_DIR/butterscotch_sdl"

    if [[ ! -f "$EXE" ]]; then
        red "$EXE not found. Run without --no-build first."
        exit 1
    fi

    # Resolve game file
    if [[ -z "$GAME_FILE" ]]; then
        for candidate in "$SCRIPT_DIR/undertale/game.unx" "$SCRIPT_DIR/game.unx"; do
            if [[ -f "$candidate" ]]; then
                GAME_FILE="$candidate"
                break
            fi
        done
        if [[ -z "$GAME_FILE" ]]; then
            red "No game file found. Use --game <path> to specify, or --no-run to skip."
            exit 1
        fi
    elif [[ ! -f "$GAME_FILE" ]]; then
        red "Game file not found: $GAME_FILE"
        exit 1
    fi

    cyan "Launching Butterscotch-C..."
    echo "    Exe:  $EXE"
    echo "    Game: $GAME_FILE"
    echo ""

    "$EXE" "$GAME_FILE"
else
    echo ""
    echo "  Executables:"
    echo "    SDL:  $BUILD_DIR/butterscotch_sdl"
    echo ""
    echo "  Run manually:"
    echo "    ./build/butterscotch_sdl <path-to-game-data>"
    echo ""
fi
