#include "bs/app.h"

#include "bs/builtin/builtin_registry.h"
#include "bs/data/form_reader.h"
#include "bs/runtime/game_runner.h"
#include "bs/vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int bs_run(const char *game_path, int frame_count) {
  bs_game_data game_data = {0};
  if (!bs_form_reader_read(game_path, &game_data)) {
    fprintf(stderr, "Failed to read game data: %s\n", game_path);
    return 1;
  }

  printf("Butterscotch-C bootstrap\n");
  printf("Game file: %s\n", game_data.game_path);
  printf("File size: %zu bytes\n", game_data.file_size);
  printf("FORM size: %u\n", game_data.form_size);
  printf("Chunks found: %zu\n", game_data.chunk_count);
  printf("Game: %s (display: %s)\n",
         game_data.gen8.game_name != NULL ? game_data.gen8.game_name : "<unknown>",
         game_data.gen8.display_name != NULL ? game_data.gen8.display_name : "<unknown>");
  printf("Window: %ux%u, room order count: %zu\n",
         game_data.gen8.window_width,
         game_data.gen8.window_height,
         game_data.gen8.room_order_count);
  printf("Parsed bootstrap chunks: STRG=%zu TPAG=%zu TXTR=%zu CODE=%zu\n",
         game_data.string_count,
         game_data.texture_page_item_count,
         game_data.texture_page_count,
         game_data.code_entry_count);
  printf("Extra bootstrap chunks: OBJT=%zu ROOM=%zu PATH=%zu SOND=%zu AUDO=%zu SCPT=%zu VARI=%zu FUNC=%zu\n",
         game_data.object_count,
         game_data.room_count,
         game_data.path_count,
         game_data.sound_count,
         game_data.audio_data_count,
         game_data.script_count,
         game_data.variable_count,
         game_data.function_count);

  bs_vm vm = {0};
  bs_vm_init(&vm, &game_data);
  bs_register_builtins(&vm);

  bs_game_runner runner = {0};
  bs_game_runner_init(&runner, &game_data, &vm);

  const char *auto_key_frame_env = getenv("BS_AUTOKEY_FRAME");
  const char *auto_key_code_env = getenv("BS_AUTOKEY_CODE");
  const char *auto_key_hold_env = getenv("BS_AUTOKEY_HOLD");
  int auto_key_frame = -1;
  int auto_key_code = 13;
  bool auto_key_hold = false;
  if (auto_key_frame_env != NULL) {
    auto_key_frame = atoi(auto_key_frame_env);
  }
  if (auto_key_code_env != NULL) {
    auto_key_code = atoi(auto_key_code_env);
  }
  if (auto_key_hold_env != NULL &&
      (strcmp(auto_key_hold_env, "1") == 0 || strcmp(auto_key_hold_env, "true") == 0)) {
    auto_key_hold = true;
  }

  if (frame_count < 1) {
    frame_count = 3;
  }

  for (int i = 0; i < frame_count && !runner.should_quit; i++) {
    if (auto_key_frame >= 0 && i == auto_key_frame) {
      bs_game_runner_on_key_down(&runner, auto_key_code);
    }
    if (!auto_key_hold && auto_key_frame >= 0 && i == auto_key_frame + 1) {
      bs_game_runner_on_key_up(&runner, auto_key_code);
    }
    bs_game_runner_step(&runner);
  }

  bs_game_runner_dispose(&runner);
  bs_vm_dispose(&vm);
  bs_game_data_free(&game_data);
  return 0;
}
