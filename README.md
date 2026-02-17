# Butterscotch-C

> A C reimplementation of the GameMaker Studio 2 Gen8 VM — built to run **Undertale**.

> [!NOTE]
> Parts of this project were *vibe coded* using **GPT 5.3 Codex** and **Claude Code Opus 4.6**.

## ⚡ Quick Setup

**🪟 Windows:** `.\setup.ps1` &nbsp; | &nbsp; **🐧 Linux:** `./setup.sh`

Both scripts install dependencies, build, and launch the game. Run with `--help` / `-?` for options.

## 🔨 Build & Run

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/butterscotch_sdl <path-to-game-data>
```

> 🪟 **Windows:** add `-DCMAKE_PREFIX_PATH="C:/msys64/ucrt64"` to the configure step if using MSYS2 for SDL2.

## 💡 Credits

Inspired by [Butterscotch](https://github.com/MrPowerGamerBR/Butterscotch) by [@MrPowerGamerBR](https://github.com/MrPowerGamerBR) (Kotlin). This is a rewrite from scratch in C.

---

<p align="center">
  <i>* Despite everything, it's still just C.</i>
</p>
