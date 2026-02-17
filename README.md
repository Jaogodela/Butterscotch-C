# Butterscotch-C

C port bootstrap for Butterscotch.

## Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

## Run

```bash
./build/butterscotch_cli undertale/game.unx
```

If no path is provided, the default is `undertale/game.unx`.

## SDL Frontend (Optional)
## GRAPHIC OPTION
If SDL2 is available during CMake configure, an interactive executable is also built:

```bash
./build/butterscotch_sdl undertale/game.unx
```

If SDL2 is not found, only `butterscotch_cli` is built.

If `SDL2_image` is also found, `butterscotch_sdl` loads `TXTR` pages and draws real sprite/font atlas data.
If `SDL2_image` is missing, it still runs with placeholder sprite/text rendering.

### Windows Note

To build `butterscotch_sdl`, CMake must be able to find SDL2 development files (`SDL2Config.cmake`).
One practical setup is MSYS2 UCRT64 with:

```bash
pacman -S --needed mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-SDL2_image
```

Then configure CMake with `CMAKE_PREFIX_PATH` pointing to that prefix (for example `C:/msys64/ucrt64`).

When running on Windows, ensure `C:\msys64\ucrt64\bin` is on `PATH` so `SDL2.dll` and `SDL2_image.dll` can be found.

