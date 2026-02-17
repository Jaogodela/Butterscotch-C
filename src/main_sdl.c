#include "bs/platform/sdl_frontend.h"

#include <stdio.h>

int main(int argc, char **argv) {
  const char *game_path = "undertale/game.unx";
  if (argc > 1 && argv[1] != NULL) {
    game_path = argv[1];
  }

  return bs_run_sdl(game_path);
}
