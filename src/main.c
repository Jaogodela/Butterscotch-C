#include "bs/app.h"

#include <stdlib.h>
#include <stdio.h>

int main(int argc, char **argv) {
  const char *game_path = "undertale/game.unx";
  int frame_count = 3;
  if (argc > 1 && argv[1] != NULL) {
    game_path = argv[1];
  }
  if (argc > 2 && argv[2] != NULL) {
    frame_count = atoi(argv[2]);
  }

  return bs_run(game_path, frame_count);
}
