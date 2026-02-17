# 🧈 Butterscotch-C

> A C reimplementation of the GameMaker Studio 2 Gen8 VM — built to run **Undertale**.

---

## ⚡ Quick Setup (Windows)

Run the setup script to install all dependencies, build, and launch the game automatically:

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

---

## 🎮 Run

```bash
./build/butterscotch_sdl undertale/game.unx
```

Place your `game.unx` inside `undertale/` and the music OGG files inside `undertale/music/`.

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
├── undertale/       # Game data (not tracked)
│   ├── game.unx
│   └── music/       # External OGG music files
├── setup.ps1        # Automated setup & build script
└── CMakeLists.txt
```

---

<p align="center">
  <i>* Despite everything, it's still just C.</i>
</p>
