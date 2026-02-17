# Butterscotch-C

C reimplementation of the GameMaker Studio 2 Gen8 VM for running Undertale.

## Quick Setup (Windows)

Run the setup script to install all dependencies, build, and launch the game:

```powershell
.\setup.ps1
```

Options:

| Flag | Description |
|------|-------------|
| `-SkipDeps` | Skip dependency installation |
| `-Clean` | Clean rebuild |
| `-NoBuild` | Skip build, just run |
| `-NoRun` | Skip running after build |
| `-GameFile "path"` | Custom game data file |

## Run

```bash
./build/butterscotch_sdl undertale/game.unx
```

Place your `game.unx` inside `undertale/` and the music OGG files inside `undertale/music/`.
