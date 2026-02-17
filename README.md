# 🧈 Butterscotch-C

> A C reimplementation of the GameMaker Studio 2 Gen8 VM — built to run **Undertale**.

---

## ⚡ Quick Setup

Run the setup script to install all dependencies, build, and launch the game automatically:

**🪟 Windows (PowerShell):**
```powershell
.\setup.ps1
```

| Flag | Description |
|------|-------------|
| `-SkipDeps` | ⏭️ Skip dependency installation |
| `-Clean` | 🧹 Clean rebuild from scratch |
| `-NoBuild` | 🚀 Skip build, just run the game |
| `-NoRun` | 🔨 Build only, don't launch |
| `-GameFile "path"` | 📂 Custom game data file |

**🐧 Linux (Debian/Ubuntu):**
```bash
chmod +x setup.sh
./setup.sh
```

| Flag | Description |
|------|-------------|
| `--skip-deps` | ⏭️ Skip dependency installation |
| `--clean` | 🧹 Clean rebuild from scratch |
| `--no-build` | 🚀 Skip build, just run the game |
| `--no-run` | 🔨 Build only, don't launch |
| `--game <path>` | 📂 Custom game data file |

---

## 🔨 Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

> 🪟 **Windows:** add `-DCMAKE_PREFIX_PATH="C:/msys64/ucrt64"` to the configure step if using MSYS2 for SDL2.

## 🎮 Run

```bash
./build/butterscotch_sdl <path-to-game-data>
```

---

## 📁 Project Structure

```
Butterscotch-C/
├── include/         # Public headers
├── src/             # Source code
│   ├── builtin/     # GML builtin functions
│   ├── data/        # Game data parsing (FORM/IFF)
│   ├── platform/    # SDL frontend
│   └── runtime/     # VM execution engine
├── setup.ps1        # Windows setup & build script
├── setup.sh         # Linux setup & build script
└── CMakeLists.txt
```

---

<p align="center">
  <i>* Despite everything, it's still just C.</i>
</p>
