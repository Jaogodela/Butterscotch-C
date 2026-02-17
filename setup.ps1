<#
.SYNOPSIS
    Setup, build and run script for Butterscotch-C.

.DESCRIPTION
    Installs all required dependencies (MSYS2, MinGW toolchain, CMake, Ninja),
    builds the project, and launches the game.

.PARAMETER SkipDeps
    Skip dependency installation and go straight to build.

.PARAMETER Clean
    Remove the build directory before configuring.

.PARAMETER NoBuild
    Skip the build step (just run the game).

.PARAMETER NoRun
    Skip running the game after build.

.PARAMETER GameFile
    Path to the game data file. Default: undertale\game.unx

.EXAMPLE
    .\setup.ps1
    .\setup.ps1 -SkipDeps
    .\setup.ps1 -Clean
    .\setup.ps1 -NoBuild
    .\setup.ps1 -NoRun
    .\setup.ps1 -GameFile "path\to\game.unx"
#>

param(
    [switch]$SkipDeps,
    [switch]$Clean,
    [switch]$NoBuild,
    [switch]$NoRun,
    [string]$GameFile = ""
)

$ErrorActionPreference = "Stop"
$ProjectRoot = $PSScriptRoot

function Write-Step($msg) {
    Write-Host "`n==> $msg" -ForegroundColor Cyan
}

function Write-Ok($msg) {
    Write-Host "    $msg" -ForegroundColor Green
}

function Write-Warn($msg) {
    Write-Host "    $msg" -ForegroundColor Yellow
}

function Test-Command($cmd) {
    $null -ne (Get-Command $cmd -ErrorAction SilentlyContinue)
}

# ---------------------------------------------------------------------------
# 1. Check / install dependencies
# ---------------------------------------------------------------------------
if (-not $SkipDeps) {

    # -- winget --
    if (-not (Test-Command "winget")) {
        Write-Host "ERROR: winget not found. Install App Installer from the Microsoft Store." -ForegroundColor Red
        exit 1
    }

    # -- CMake --
    Write-Step "Checking CMake..."
    if (Test-Command "cmake") {
        Write-Ok "cmake found: $(cmake --version | Select-Object -First 1)"
    } else {
        Write-Warn "cmake not found, installing via winget..."
        winget install --id Kitware.CMake --accept-package-agreements --accept-source-agreements
        $env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                     [System.Environment]::GetEnvironmentVariable("Path", "User")
    }

    # -- MinGW (GCC + Ninja) --
    Write-Step "Checking MinGW toolchain (gcc, ninja)..."
    if (Test-Command "gcc") {
        Write-Ok "gcc found: $(gcc --version | Select-Object -First 1)"
    } else {
        Write-Warn "gcc not found, installing WinLibs MinGW via winget..."
        winget install --id BrechtSanders.WinLibs.POSIX.UCRT --accept-package-agreements --accept-source-agreements
        $env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                     [System.Environment]::GetEnvironmentVariable("Path", "User")
    }

    if (-not (Test-Command "ninja")) {
        Write-Warn "ninja not found, installing via winget..."
        winget install --id Ninja-build.Ninja --accept-package-agreements --accept-source-agreements
        $env:Path = [System.Environment]::GetEnvironmentVariable("Path", "Machine") + ";" +
                     [System.Environment]::GetEnvironmentVariable("Path", "User")
    } else {
        Write-Ok "ninja found: $(ninja --version)"
    }

    # -- MSYS2 + SDL2 packages --
    Write-Step "Checking MSYS2 and SDL2 packages..."
    $msys2Root = "C:\msys64"
    if (-not (Test-Path "$msys2Root\usr\bin\bash.exe")) {
        Write-Warn "MSYS2 not found at $msys2Root, installing via winget..."
        winget install --id MSYS2.MSYS2 --accept-package-agreements --accept-source-agreements
    }

    if (Test-Path "$msys2Root\usr\bin\bash.exe") {
        $sdlPackages = @(
            "mingw-w64-ucrt-x86_64-SDL2",
            "mingw-w64-ucrt-x86_64-SDL2_image",
            "mingw-w64-ucrt-x86_64-SDL2_mixer"
        )

        # Check which packages are missing
        $missing = @()
        foreach ($pkg in $sdlPackages) {
            $check = & "$msys2Root\usr\bin\bash.exe" -lc "pacman -Q $pkg 2>/dev/null"
            if ($LASTEXITCODE -ne 0) {
                $missing += $pkg
            } else {
                Write-Ok "$check"
            }
        }

        if ($missing.Count -gt 0) {
            Write-Warn "Installing missing SDL2 packages: $($missing -join ', ')"
            $pkgList = $missing -join " "
            & "$msys2Root\usr\bin\bash.exe" -lc "pacman -S --noconfirm --needed $pkgList"
            if ($LASTEXITCODE -ne 0) {
                Write-Host "ERROR: Failed to install SDL2 packages." -ForegroundColor Red
                exit 1
            }
        }

        Write-Ok "All SDL2 packages installed."
    } else {
        Write-Host "ERROR: MSYS2 installation failed or not found at $msys2Root" -ForegroundColor Red
        exit 1
    }
}

# ---------------------------------------------------------------------------
# 2. Configure and build
# ---------------------------------------------------------------------------
$buildDir = Join-Path $ProjectRoot "build"

if (-not $NoBuild) {
    if ($Clean -and (Test-Path $buildDir)) {
        Write-Step "Cleaning build directory..."
        Remove-Item -Recurse -Force $buildDir
    }

    Write-Step "Configuring with CMake..."
    cmake -S $ProjectRoot -B $buildDir -G Ninja `
          -DCMAKE_PREFIX_PATH="C:/msys64/ucrt64"

    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: CMake configure failed." -ForegroundColor Red
        exit 1
    }

    Write-Step "Building..."
    cmake --build $buildDir

    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Build failed." -ForegroundColor Red
        exit 1
    }

    Write-Host ""
    Write-Host "================================================" -ForegroundColor Green
    Write-Host "  Build complete!" -ForegroundColor Green
    Write-Host "================================================" -ForegroundColor Green
}

# ---------------------------------------------------------------------------
# 3. Run the game
# ---------------------------------------------------------------------------
if (-not $NoRun) {
    $exe = Join-Path $buildDir "butterscotch_sdl.exe"

    if (-not (Test-Path $exe)) {
        Write-Host "ERROR: $exe not found. Run without -NoBuild first." -ForegroundColor Red
        exit 1
    }

    # Resolve the game file
    if ($GameFile -eq "") {
        # Auto-detect: look in common locations
        $candidates = @(
            (Join-Path $ProjectRoot "undertale\game.unx"),
            (Join-Path $ProjectRoot "game.unx")
        )
        foreach ($c in $candidates) {
            if (Test-Path $c) {
                $GameFile = $c
                break
            }
        }
        if ($GameFile -eq "") {
            Write-Host "ERROR: No game file found. Searched:" -ForegroundColor Red
            foreach ($c in $candidates) {
                Write-Host "    $c" -ForegroundColor Red
            }
            Write-Host "Use -GameFile to specify the path, or -NoRun to skip." -ForegroundColor Yellow
            exit 1
        }
    } elseif (-not (Test-Path $GameFile)) {
        Write-Host "ERROR: Game file not found: $GameFile" -ForegroundColor Red
        exit 1
    }

    Write-Step "Launching Butterscotch-C..."
    Write-Host "    Exe:  $exe"
    Write-Host "    Game: $GameFile"
    Write-Host ""

    & $exe $GameFile
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        Write-Host "Game exited with code $exitCode" -ForegroundColor Yellow
    }
} else {
    # No-run mode: just show how to run manually
    Write-Host ""
    Write-Host "  Executables:" -ForegroundColor White
    Write-Host "    CLI:  $buildDir\butterscotch_cli.exe"
    if (Test-Path "$buildDir\butterscotch_sdl.exe") {
        Write-Host "    SDL:  $buildDir\butterscotch_sdl.exe"
    }
    Write-Host ""
    Write-Host "  Run manually:" -ForegroundColor White
    Write-Host "    .\build\butterscotch_sdl.exe undertale\game.unx"
    Write-Host ""
}
