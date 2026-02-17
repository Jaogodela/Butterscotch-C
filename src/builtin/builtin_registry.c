#include "bs/builtin/builtin_registry.h"

#include "bs/runtime/game_runner.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int32_t g_next_ds_map_id = 1;
static char g_temp_strings[32][512];
static size_t g_temp_string_index = 0;
static unsigned int g_rng_state = 0xC0FFEEu;

typedef struct bs_ds_map_value {
  bool is_string;
  double number;
  char *string;
} bs_ds_map_value;

typedef struct bs_ds_map_entry {
  char *key;
  bs_ds_map_value value;
} bs_ds_map_entry;

typedef struct bs_ds_map {
  int32_t id;
  bs_ds_map_entry *entries;
  size_t entry_count;
  size_t entry_capacity;
} bs_ds_map;

static bs_ds_map *g_ds_maps = NULL;
static size_t g_ds_map_count = 0;
static size_t g_ds_map_capacity = 0;

static char *bs_builtin_dup_string(const char *value) {
  size_t len = 0;
  char *copy = NULL;
  if (value == NULL) {
    value = "";
  }
  len = strlen(value);
  copy = (char *)malloc(len + 1u);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, value, len + 1u);
  return copy;
}

static void bs_ds_map_value_dispose(bs_ds_map_value *value) {
  if (value == NULL) {
    return;
  }
  free(value->string);
  value->string = NULL;
  value->number = 0.0;
  value->is_string = false;
}

static bs_ds_map *bs_ds_map_find(int32_t map_id) {
  for (size_t i = 0; i < g_ds_map_count; i++) {
    if (g_ds_maps[i].id == map_id) {
      return &g_ds_maps[i];
    }
  }
  return NULL;
}

static bool bs_ds_map_set_value(int32_t map_id, const char *key, bs_vm_value value) {
  bs_ds_map *map = bs_ds_map_find(map_id);
  char *key_copy = NULL;
  if (map == NULL || key == NULL) {
    return false;
  }

  for (size_t i = 0; i < map->entry_count; i++) {
    if (strcmp(map->entries[i].key, key) == 0) {
      bs_ds_map_value_dispose(&map->entries[i].value);
      if (value.type == BS_VM_VALUE_STRING) {
        map->entries[i].value.is_string = true;
        map->entries[i].value.string = bs_builtin_dup_string(value.string != NULL ? value.string : "");
        map->entries[i].value.number = 0.0;
      } else {
        map->entries[i].value.is_string = false;
        map->entries[i].value.number = value.number;
        map->entries[i].value.string = NULL;
      }
      return true;
    }
  }

  if (map->entry_count == map->entry_capacity) {
    size_t new_capacity = (map->entry_capacity == 0) ? 16u : (map->entry_capacity * 2u);
    bs_ds_map_entry *grown = (bs_ds_map_entry *)realloc(map->entries, new_capacity * sizeof(bs_ds_map_entry));
    if (grown == NULL) {
      return false;
    }
    map->entries = grown;
    map->entry_capacity = new_capacity;
  }

  key_copy = bs_builtin_dup_string(key);
  if (key_copy == NULL) {
    return false;
  }

  map->entries[map->entry_count].key = key_copy;
  map->entries[map->entry_count].value.is_string = false;
  map->entries[map->entry_count].value.number = 0.0;
  map->entries[map->entry_count].value.string = NULL;

  if (value.type == BS_VM_VALUE_STRING) {
    map->entries[map->entry_count].value.is_string = true;
    map->entries[map->entry_count].value.string = bs_builtin_dup_string(value.string != NULL ? value.string : "");
  } else {
    map->entries[map->entry_count].value.number = value.number;
  }

  map->entry_count++;
  return true;
}

static bool bs_ds_map_find_value(int32_t map_id, const char *key, bs_ds_map_value *out_value) {
  bs_ds_map *map = bs_ds_map_find(map_id);
  if (map == NULL || key == NULL || out_value == NULL) {
    return false;
  }

  for (size_t i = 0; i < map->entry_count; i++) {
    if (strcmp(map->entries[i].key, key) == 0) {
      *out_value = map->entries[i].value;
      return true;
    }
  }

  return false;
}

static const char *bs_builtin_store_temp_string(const char *value) {
  size_t slot = 0;
  if (value == NULL) {
    value = "";
  }

  slot = g_temp_string_index % 32u;
  g_temp_string_index++;
  (void)snprintf(g_temp_strings[slot], sizeof(g_temp_strings[slot]), "%s", value);
  return g_temp_strings[slot];
}

static double bs_builtin_rand01(void) {
  g_rng_state = (1103515245u * g_rng_state + 12345u);
  return (double)(g_rng_state & 0x7FFFFFFFu) / 2147483647.0;
}

static double bs_builtin_now_millis(void) {
  struct timespec ts;
#if defined(TIME_UTC)
  if (timespec_get(&ts, TIME_UTC) == TIME_UTC) {
    return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1000000.0);
  }
#endif
  return (double)time(NULL) * 1000.0;
}

static const char *bs_builtin_arg_to_string(const bs_vm_value *args,
                                            size_t argc,
                                            size_t index,
                                            char *scratch,
                                            size_t scratch_size) {
  if (args == NULL || index >= argc) {
    if (scratch != NULL && scratch_size > 0) {
      scratch[0] = '\0';
    }
    return "";
  }

  if (args[index].type == BS_VM_VALUE_STRING) {
    return args[index].string != NULL ? args[index].string : "";
  }

  if (scratch != NULL && scratch_size > 0) {
    (void)snprintf(scratch, scratch_size, "%g", args[index].number);
    return scratch;
  }

  return "";
}

static double bs_builtin_arg_to_number(const bs_vm_value *args, size_t argc, size_t index, double fallback) {
  if (args == NULL || index >= argc) {
    return fallback;
  }
  if (args[index].type == BS_VM_VALUE_STRING) {
    const char *s = args[index].string != NULL ? args[index].string : "";
    return strtod(s, NULL);
  }
  return args[index].number;
}

static double bs_builtin_value_to_number(bs_vm_value value) {
  if (value.type == BS_VM_VALUE_STRING) {
    const char *s = value.string != NULL ? value.string : "";
    return strtod(s, NULL);
  }
  return value.number;
}

static int32_t bs_builtin_color_to_u24(double value) {
  int64_t iv = (int64_t)value;
  if (iv < 0) {
    iv = 0;
  }
  if (iv > 0xFFFFFF) {
    iv = 0xFFFFFF;
  }
  return (int32_t)iv;
}

static int32_t bs_builtin_alpha01_to_u8(double value) {
  int64_t iv = (int64_t)round(value * 255.0);
  if (iv < 0) {
    iv = 0;
  }
  if (iv > 255) {
    iv = 255;
  }
  return (int32_t)iv;
}

static bool bs_builtin_any_key_state(const bool *state) {
  if (state == NULL) {
    return false;
  }
  for (int i = 0; i < 256; i++) {
    if (state[i]) {
      return true;
    }
  }
  return false;
}

static bs_instance *bs_builtin_get_self_instance(bs_vm *vm) {
  if (vm == NULL || vm->runner == NULL || vm->current_self_id < 0) {
    return NULL;
  }
  return bs_game_runner_find_instance_by_id(vm->runner, vm->current_self_id);
}

static bool bs_builtin_instance_matches_target(const bs_game_runner *runner,
                                               const bs_instance *instance,
                                               int32_t target) {
  if (runner == NULL || instance == NULL || instance->destroyed) {
    return false;
  }
  if (target >= 100000) {
    return instance->id == target;
  }
  return bs_game_runner_object_is_child_of(runner, instance->object_index, target);
}

static bool bs_builtin_interpolate_path_position(const bs_game_data *game_data,
                                                 int32_t path_index,
                                                 double position,
                                                 double *out_x,
                                                 double *out_y) {
  const bs_path_data *path = NULL;
  size_t seg_count = 0;
  double total = 0.0;
  double clamped = 0.0;
  double target = 0.0;
  double accumulated = 0.0;
  if (game_data == NULL || out_x == NULL || out_y == NULL) {
    return false;
  }
  if (path_index < 0 || (size_t)path_index >= game_data->path_count) {
    return false;
  }

  path = &game_data->paths[(size_t)path_index];
  if (path->point_count == 0) {
    return false;
  }
  if (path->point_count == 1) {
    *out_x = (double)path->points[0].x;
    *out_y = (double)path->points[0].y;
    return true;
  }

  seg_count = path->is_closed ? path->point_count : (path->point_count - 1u);
  for (size_t i = 0; i < seg_count; i++) {
    const bs_path_point_data *p1 = &path->points[i];
    const bs_path_point_data *p2 = &path->points[(i + 1u) % path->point_count];
    double dx = (double)p2->x - (double)p1->x;
    double dy = (double)p2->y - (double)p1->y;
    total += sqrt((dx * dx) + (dy * dy));
  }

  if (total <= 0.0) {
    *out_x = (double)path->points[0].x;
    *out_y = (double)path->points[0].y;
    return true;
  }

  clamped = position;
  if (clamped < 0.0) {
    clamped = 0.0;
  } else if (clamped > 1.0) {
    clamped = 1.0;
  }
  target = clamped * total;

  for (size_t i = 0; i < seg_count; i++) {
    const bs_path_point_data *p1 = &path->points[i];
    const bs_path_point_data *p2 = &path->points[(i + 1u) % path->point_count];
    double dx = (double)p2->x - (double)p1->x;
    double dy = (double)p2->y - (double)p1->y;
    double seg_length = sqrt((dx * dx) + (dy * dy));
    if ((accumulated + seg_length) >= target || i + 1 == seg_count) {
      double seg_t = 0.0;
      if (seg_length > 0.0) {
        seg_t = (target - accumulated) / seg_length;
      }
      *out_x = (double)p1->x + (((double)p2->x - (double)p1->x) * seg_t);
      *out_y = (double)p1->y + (((double)p2->y - (double)p1->y) * seg_t);
      return true;
    }
    accumulated += seg_length;
  }

  *out_x = (double)path->points[path->point_count - 1u].x;
  *out_y = (double)path->points[path->point_count - 1u].y;
  return true;
}

static int32_t bs_builtin_find_global_variable_index_by_name(const bs_vm *vm, const char *name) {
  if (vm == NULL || vm->game_data == NULL || name == NULL) {
    return -1;
  }

  for (size_t i = 0; i < vm->game_data->variable_count; i++) {
    const bs_variable_data *var = &vm->game_data->variables[i];
    if (var->instance_type == BS_INSTANCE_GLOBAL &&
        var->name != NULL &&
        strcmp(var->name, name) == 0) {
      return (int32_t)i;
    }
  }

  return -1;
}

static bool bs_builtin_global_exists_by_index(const bs_vm *vm, int32_t variable_index) {
  if (vm == NULL || variable_index < 0) {
    return false;
  }

  for (size_t i = 0; i < vm->global_variable_count; i++) {
    if (vm->global_variable_indices[i] == variable_index) {
      return true;
    }
  }
  return false;
}

static double bs_builtin_global_get_by_index(const bs_vm *vm, int32_t variable_index) {
  if (vm == NULL || variable_index < 0) {
    return 0.0;
  }

  for (size_t i = 0; i < vm->global_variable_count; i++) {
    if (vm->global_variable_indices[i] == variable_index) {
      return bs_builtin_value_to_number(vm->global_variable_values[i]);
    }
  }
  return 0.0;
}

static bool bs_builtin_global_set_by_index(bs_vm *vm, int32_t variable_index, double value) {
  if (vm == NULL || variable_index < 0) {
    return false;
  }

  for (size_t i = 0; i < vm->global_variable_count; i++) {
    if (vm->global_variable_indices[i] == variable_index) {
      vm->global_variable_values[i] = bs_vm_make_number(value);
      return true;
    }
  }

  if (vm->global_variable_count == vm->global_variable_capacity) {
    size_t new_capacity = (vm->global_variable_capacity == 0) ? 128u : (vm->global_variable_capacity * 2u);
    int32_t *grown_indices = (int32_t *)malloc(new_capacity * sizeof(int32_t));
    bs_vm_value *grown_values = (bs_vm_value *)malloc(new_capacity * sizeof(bs_vm_value));
    if (grown_indices == NULL || grown_values == NULL) {
      free(grown_indices);
      free(grown_values);
      return false;
    }

    if (vm->global_variable_count > 0) {
      memcpy(grown_indices, vm->global_variable_indices, vm->global_variable_count * sizeof(int32_t));
      memcpy(grown_values, vm->global_variable_values, vm->global_variable_count * sizeof(bs_vm_value));
    }

    free(vm->global_variable_indices);
    free(vm->global_variable_values);
    vm->global_variable_indices = grown_indices;
    vm->global_variable_values = grown_values;
    vm->global_variable_capacity = new_capacity;
  }

  vm->global_variable_indices[vm->global_variable_count] = variable_index;
  vm->global_variable_values[vm->global_variable_count] = bs_vm_make_number(value);
  vm->global_variable_count++;
  return true;
}

static bs_vm_value bs_builtin_show_debug_message(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char scratch[64];
  const char *text = bs_builtin_arg_to_string(args, argc, 0, scratch, sizeof(scratch));
  (void)vm;
  printf("[DEBUG] %s\n", text);
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_room_goto(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm != NULL && vm->runner != NULL && argc > 0) {
    vm->runner->pending_room_goto = (int32_t)bs_builtin_arg_to_number(args, argc, 0, 0.0);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_room_goto_next(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm != NULL && vm->runner != NULL) {
    vm->runner->pending_room_goto = vm->runner->current_room_index + 1;
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_room_goto_previous(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm != NULL && vm->runner != NULL) {
    vm->runner->pending_room_goto = vm->runner->current_room_index - 1;
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_instance_create(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  bs_instance *created = NULL;
  if (vm == NULL || vm->runner == NULL || argc < 3) {
    return bs_vm_make_number(-4.0);
  }

  created = bs_game_runner_create_instance_runtime(vm->runner,
                                                   (int32_t)bs_builtin_arg_to_number(args, argc, 2, -1.0),
                                                   bs_builtin_arg_to_number(args, argc, 0, 0.0),
                                                   bs_builtin_arg_to_number(args, argc, 1, 0.0),
                                                   true);
  if (created == NULL) {
    return bs_vm_make_number(-4.0);
  }
  return bs_vm_make_number((double)created->id);
}

static bs_vm_value bs_builtin_instance_destroy(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t target_id = -4;
  if (vm == NULL || vm->runner == NULL) {
    return bs_vm_make_number(0.0);
  }

  if (argc > 0) {
    target_id = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -4.0);
  } else {
    target_id = vm->current_self_id;
  }

  bs_game_runner_destroy_instance(vm->runner, target_id);
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_instance_exists(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || argc < 1) {
    return bs_vm_make_number(0.0);
  }

  {
    int32_t target = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -4.0);
    if (target >= 100000) {
      bs_instance *inst = bs_game_runner_find_instance_by_id(vm->runner, target);
      return bs_vm_make_number((inst != NULL && !inst->destroyed) ? 1.0 : 0.0);
    }

    for (size_t i = 0; i < vm->runner->instance_count; i++) {
      if (!vm->runner->instances[i].destroyed &&
          bs_game_runner_object_is_child_of(vm->runner, vm->runner->instances[i].object_index, target)) {
        return bs_vm_make_number(1.0);
      }
    }
  }

  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_instance_number(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t object_index = 0;
  double count = 0.0;
  if (vm == NULL || vm->runner == NULL || argc < 1) {
    return bs_vm_make_number(0.0);
  }

  object_index = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  for (size_t i = 0; i < vm->runner->instance_count; i++) {
    if (!vm->runner->instances[i].destroyed &&
        bs_game_runner_object_is_child_of(vm->runner, vm->runner->instances[i].object_index, object_index)) {
      count += 1.0;
    }
  }
  return bs_vm_make_number(count);
}

static bs_vm_value bs_builtin_instance_find(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t object_index = 0;
  int32_t target_n = 0;
  int32_t current_n = 0;
  if (vm == NULL || vm->runner == NULL || argc < 2) {
    return bs_vm_make_number(-4.0);
  }

  object_index = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  target_n = (int32_t)bs_builtin_arg_to_number(args, argc, 1, 0.0);
  for (size_t i = 0; i < vm->runner->instance_count; i++) {
    if (!vm->runner->instances[i].destroyed &&
        bs_game_runner_object_is_child_of(vm->runner, vm->runner->instances[i].object_index, object_index)) {
      if (current_n == target_n) {
        return bs_vm_make_number((double)vm->runner->instances[i].id);
      }
      current_n++;
    }
  }
  return bs_vm_make_number(-4.0);
}

static bs_vm_value bs_builtin_path_start(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  bs_instance *self = bs_builtin_get_self_instance(vm);
  int32_t path_index = 0;
  double speed = 0.0;
  int32_t end_action = 0;
  bool absolute = false;
  double start_x = 0.0;
  double start_y = 0.0;
  if (vm == NULL || vm->runner == NULL || vm->game_data == NULL || self == NULL || argc < 4) {
    return bs_vm_make_number(0.0);
  }

  path_index = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  speed = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  end_action = (int32_t)bs_builtin_arg_to_number(args, argc, 2, 0.0);
  absolute = bs_builtin_arg_to_number(args, argc, 3, 0.0) != 0.0;

  self->path_index = path_index;
  self->path_speed = speed;
  self->path_end_action = end_action;
  self->path_position = 0.0;

  if (bs_builtin_interpolate_path_position(vm->game_data, path_index, 0.0, &start_x, &start_y)) {
    if (absolute) {
      self->path_x_offset = 0.0;
      self->path_y_offset = 0.0;
      self->x = start_x;
      self->y = start_y;
    } else {
      self->path_x_offset = self->x - start_x;
      self->path_y_offset = self->y - start_y;
    }
  }

  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_path_end(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  bs_instance *self = bs_builtin_get_self_instance(vm);
  (void)args;
  (void)argc;
  if (vm != NULL && vm->runner != NULL && self != NULL) {
    bs_game_runner_path_end_instance(vm->runner, self);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_os_get_language(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_string("en");
}

static bs_vm_value bs_builtin_os_get_region(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_string("US");
}

static bs_vm_value bs_builtin_randomize(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  g_rng_state = (unsigned int)time(NULL);
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_random_set_seed(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  uint64_t seed = (uint64_t)bs_builtin_arg_to_number(args, argc, 0, 0.0);
  (void)vm;
  g_rng_state = (unsigned int)(seed & 0xFFFFFFFFu);
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_game_end(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm != NULL && vm->runner != NULL) {
    vm->runner->should_quit = true;
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_audio_channel_num(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(128.0);
}

static bs_vm_value bs_builtin_steam_initialised(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_joystick_exists(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_sprite_prefetch(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_window_set_caption(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_window_get_width(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm != NULL && vm->runner != NULL && vm->runner->surface_width > 0) {
    return bs_vm_make_number((double)vm->runner->surface_width);
  }
  return bs_vm_make_number(640.0);
}

static bs_vm_value bs_builtin_window_get_height(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm != NULL && vm->runner != NULL && vm->runner->surface_height > 0) {
    return bs_vm_make_number((double)vm->runner->surface_height);
  }
  return bs_vm_make_number(480.0);
}

static bs_vm_value bs_builtin_window_get_caption(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm != NULL &&
      vm->game_data != NULL &&
      vm->game_data->gen8.display_name != NULL &&
      vm->game_data->gen8.display_name[0] != '\0') {
    return bs_vm_make_string(vm->game_data->gen8.display_name);
  }
  return bs_vm_make_string("UNDERTALE");
}

static bs_vm_value bs_builtin_window_set_fullscreen(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_window_get_fullscreen(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_display_get_width(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm != NULL && vm->game_data != NULL && vm->game_data->gen8.window_width > 0u) {
    return bs_vm_make_number((double)vm->game_data->gen8.window_width);
  }
  return bs_vm_make_number(640.0);
}

static bs_vm_value bs_builtin_display_get_height(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm != NULL && vm->game_data != NULL && vm->game_data->gen8.window_height > 0u) {
    return bs_vm_make_number((double)vm->game_data->gen8.window_height);
  }
  return bs_vm_make_number(480.0);
}

static bs_vm_value bs_builtin_display_set_gui_size(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_view_set_visible(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_application_surface_enable(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_application_surface_draw_enable(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_gamepad_get_device_count(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_gamepad_is_connected(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_keyboard_key_press(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t key = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  if (vm != NULL && vm->runner != NULL && key >= 0 && key < 256) {
    vm->runner->keys_pressed[key] = true;
    vm->runner->keys_held[key] = true;
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_keyboard_key_release(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t key = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  if (vm != NULL && vm->runner != NULL && key >= 0 && key < 256) {
    vm->runner->keys_released[key] = true;
    vm->runner->keys_held[key] = false;
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_keyboard_clear(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL) {
    return bs_vm_make_number(0.0);
  }
  if (argc < 1) {
    memset(vm->runner->keys_held, 0, sizeof(vm->runner->keys_held));
    memset(vm->runner->keys_pressed, 0, sizeof(vm->runner->keys_pressed));
    memset(vm->runner->keys_released, 0, sizeof(vm->runner->keys_released));
    return bs_vm_make_number(0.0);
  }
  {
    int32_t key = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
    if (key >= 0 && key < 256) {
      vm->runner->keys_held[key] = false;
      vm->runner->keys_pressed[key] = false;
      vm->runner->keys_released[key] = false;
    }
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_current_time_fn(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(bs_builtin_now_millis());
}

static bs_vm_value bs_builtin_date_current_datetime(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number((bs_builtin_now_millis() / 86400000.0) + 25569.0);
}

static bs_vm_value bs_builtin_get_timer(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  struct timespec ts;
  (void)vm;
  (void)args;
  (void)argc;
#if defined(TIME_UTC)
  if (timespec_get(&ts, TIME_UTC) == TIME_UTC) {
    return bs_vm_make_number(((double)ts.tv_sec * 1000000.0) + ((double)ts.tv_nsec / 1000.0));
  }
#endif
  return bs_vm_make_number(((double)clock() * 1000000.0) / (double)CLOCKS_PER_SEC);
}

static bs_vm_value bs_builtin_environment_get_variable(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_string("");
}

static bs_vm_value bs_builtin_parameter_count(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_parameter_string(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_string("");
}

static bs_vm_value bs_builtin_show_message(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char scratch[256];
  const char *msg = bs_builtin_arg_to_string(args, argc, 0, scratch, sizeof(scratch));
  (void)vm;
  printf("[MESSAGE] %s\n", msg);
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_game_restart(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_steam_stats_ready(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_real(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  return bs_vm_make_number(bs_builtin_arg_to_number(args, argc, 0, 0.0));
}

static bs_vm_value bs_builtin_string_cast(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char scratch[128];
  const char *value = bs_builtin_arg_to_string(args, argc, 0, scratch, sizeof(scratch));
  (void)vm;
  return bs_vm_make_string(bs_builtin_store_temp_string(value));
}

static bs_vm_value bs_builtin_chr(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t code = (int32_t)bs_builtin_arg_to_number(args, argc, 0, 0.0);
  char out[2];
  (void)vm;
  out[0] = (char)(code & 0xFF);
  out[1] = '\0';
  return bs_vm_make_string(bs_builtin_store_temp_string(out));
}

static bs_vm_value bs_builtin_ord(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char scratch[128];
  const char *value = bs_builtin_arg_to_string(args, argc, 0, scratch, sizeof(scratch));
  (void)vm;
  if (value[0] == '\0') {
    return bs_vm_make_number(0.0);
  }
  return bs_vm_make_number((double)(unsigned char)value[0]);
}

static bs_vm_value bs_builtin_ansi_char(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  return bs_builtin_chr(vm, args, argc);
}

static bs_vm_value bs_builtin_string_upper(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char scratch[128];
  const char *text = bs_builtin_arg_to_string(args, argc, 0, scratch, sizeof(scratch));
  char out[512];
  (void)vm;

  size_t len = strlen(text);
  if (len >= sizeof(out)) {
    len = sizeof(out) - 1u;
  }
  for (size_t i = 0; i < len; i++) {
    out[i] = (char)toupper((unsigned char)text[i]);
  }
  out[len] = '\0';
  return bs_vm_make_string(bs_builtin_store_temp_string(out));
}

static bs_vm_value bs_builtin_string_lower(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char scratch[128];
  const char *text = bs_builtin_arg_to_string(args, argc, 0, scratch, sizeof(scratch));
  char out[512];
  size_t len = strlen(text);
  (void)vm;

  if (len >= sizeof(out)) {
    len = sizeof(out) - 1u;
  }
  for (size_t i = 0; i < len; i++) {
    out[i] = (char)tolower((unsigned char)text[i]);
  }
  out[len] = '\0';
  return bs_vm_make_string(bs_builtin_store_temp_string(out));
}

static bs_vm_value bs_builtin_string_length(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char scratch[128];
  const char *text = bs_builtin_arg_to_string(args, argc, 0, scratch, sizeof(scratch));
  (void)vm;
  return bs_vm_make_number((double)strlen(text));
}

static bs_vm_value bs_builtin_string_pos(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char needle_scratch[256];
  char haystack_scratch[512];
  const char *needle = bs_builtin_arg_to_string(args, argc, 0, needle_scratch, sizeof(needle_scratch));
  const char *haystack = bs_builtin_arg_to_string(args, argc, 1, haystack_scratch, sizeof(haystack_scratch));
  const char *found = NULL;
  (void)vm;

  if (needle[0] == '\0') {
    return bs_vm_make_number(1.0);
  }

  found = strstr(haystack, needle);
  if (found == NULL) {
    return bs_vm_make_number(0.0);
  }
  return bs_vm_make_number((double)((found - haystack) + 1));
}

static const bs_font_data *bs_builtin_current_font(const bs_vm *vm) {
  int32_t font_index = 0;
  if (vm == NULL || vm->runner == NULL || vm->game_data == NULL) {
    return NULL;
  }
  font_index = vm->runner->draw_font_index;
  if (font_index < 0 || (size_t)font_index >= vm->game_data->font_count) {
    return NULL;
  }
  return &vm->game_data->fonts[(size_t)font_index];
}

static const bs_font_glyph_data *bs_builtin_find_glyph_ascii(const bs_font_data *font, uint8_t ch) {
  if (font == NULL || font->glyphs == NULL) {
    return NULL;
  }
  for (size_t i = 0; i < font->glyph_count; i++) {
    if (font->glyphs[i].character == (uint16_t)ch) {
      return &font->glyphs[i];
    }
  }
  return NULL;
}

static double bs_builtin_measure_line_width_ascii(const bs_font_data *font, const char *line, size_t len) {
  double width = 0.0;
  double fallback_advance = 8.0;
  if (font != NULL && font->em_size > 0) {
    fallback_advance = (double)font->em_size * 0.5;
  }
  for (size_t i = 0; i < len; i++) {
    uint8_t ch = (uint8_t)line[i];
    const bs_font_glyph_data *glyph = NULL;
    if (ch == '\t') {
      width += fallback_advance * 4.0;
      continue;
    }
    if (ch < 32u) {
      continue;
    }
    glyph = bs_builtin_find_glyph_ascii(font, ch);
    if (glyph != NULL) {
      if (glyph->shift > 0) {
        width += (double)glyph->shift;
      } else if (glyph->width > 0) {
        width += (double)glyph->width;
      } else {
        width += fallback_advance;
      }
    } else {
      width += fallback_advance;
    }
  }
  return width;
}

static bs_vm_value bs_builtin_string_width(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char text_scratch[1024];
  const char *text = bs_builtin_arg_to_string(args, argc, 0, text_scratch, sizeof(text_scratch));
  const bs_font_data *font = bs_builtin_current_font(vm);
  const char *line_start = text;
  const char *p = text;
  double max_width = 0.0;

  while (*p != '\0') {
    if (*p == '\n') {
      size_t line_len = (size_t)(p - line_start);
      double line_width = bs_builtin_measure_line_width_ascii(font, line_start, line_len);
      if (line_width > max_width) {
        max_width = line_width;
      }
      line_start = p + 1;
    }
    p++;
  }

  {
    size_t line_len = (size_t)(p - line_start);
    double line_width = bs_builtin_measure_line_width_ascii(font, line_start, line_len);
    if (line_width > max_width) {
      max_width = line_width;
    }
  }

  return bs_vm_make_number(max_width);
}

static bs_vm_value bs_builtin_string_height(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char text_scratch[1024];
  const char *text = bs_builtin_arg_to_string(args, argc, 0, text_scratch, sizeof(text_scratch));
  const bs_font_data *font = bs_builtin_current_font(vm);
  int32_t line_count = 1;
  double line_height = 16.0;

  if (font != NULL && font->em_size > 0) {
    line_height = (double)font->em_size;
  }

  for (const char *p = text; *p != '\0'; p++) {
    if (*p == '\n') {
      line_count++;
    }
  }

  return bs_vm_make_number((double)line_count * line_height);
}

static bs_vm_value bs_builtin_round(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double value = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  (void)vm;
  if (value >= 0.0) {
    return bs_vm_make_number(floor(value + 0.5));
  }
  return bs_vm_make_number(ceil(value - 0.5));
}

static bs_vm_value bs_builtin_string_char_at(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char text_scratch[256];
  const char *text = bs_builtin_arg_to_string(args, argc, 0, text_scratch, sizeof(text_scratch));
  int32_t index = (int32_t)bs_builtin_arg_to_number(args, argc, 1, 1.0);
  char out[2];
  size_t len = strlen(text);
  (void)vm;

  out[0] = '\0';
  out[1] = '\0';
  if (index >= 1 && (size_t)index <= len) {
    out[0] = text[(size_t)index - 1u];
  }
  return bs_vm_make_string(bs_builtin_store_temp_string(out));
}

static bs_vm_value bs_builtin_string_copy(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char text_scratch[512];
  const char *text = bs_builtin_arg_to_string(args, argc, 0, text_scratch, sizeof(text_scratch));
  int32_t index = (int32_t)bs_builtin_arg_to_number(args, argc, 1, 1.0);
  int32_t length = (int32_t)bs_builtin_arg_to_number(args, argc, 2, 0.0);
  char out[2048];
  size_t text_len = strlen(text);
  size_t start = 0;
  size_t copy_len = 0;
  static int trace_init = 0;
  static bool trace_enabled = false;
  static int trace_count = 0;
  (void)vm;

  if (!trace_init) {
    const char *env = getenv("BS_TRACE_STRING_COPY");
    trace_enabled = (env != NULL && (strcmp(env, "1") == 0 || strcmp(env, "true") == 0));
    trace_init = 1;
  }

  out[0] = '\0';

  if (index > 1) {
    start = (size_t)(index - 1);
  }

  if (length <= 0 || start >= text_len) {
    if (trace_enabled && trace_count < 200) {
      printf("  [STRING_COPY] idx=%d len=%d text=\"%s\" -> \"\"\n", index, length, text);
      trace_count++;
    }
    return bs_vm_make_string(bs_builtin_store_temp_string(out));
  }

  copy_len = (size_t)length;
  if (start + copy_len > text_len) {
    copy_len = text_len - start;
  }
  if (copy_len >= sizeof(out)) {
    copy_len = sizeof(out) - 1u;
  }

  memcpy(out, text + start, copy_len);
  out[copy_len] = '\0';
  if (trace_enabled && trace_count < 200) {
    printf("  [STRING_COPY] idx=%d len=%d text=\"%s\" -> \"%s\"\n", index, length, text, out);
    trace_count++;
  }
  return bs_vm_make_string(bs_builtin_store_temp_string(out));
}

static bs_vm_value bs_builtin_string_replace_all(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char src_scratch[256];
  char find_scratch[128];
  char repl_scratch[128];
  const char *source = bs_builtin_arg_to_string(args, argc, 0, src_scratch, sizeof(src_scratch));
  const char *find = bs_builtin_arg_to_string(args, argc, 1, find_scratch, sizeof(find_scratch));
  const char *replace = bs_builtin_arg_to_string(args, argc, 2, repl_scratch, sizeof(repl_scratch));
  char out[2048];
  size_t out_len = 0;
  size_t find_len = strlen(find);
  size_t replace_len = strlen(replace);
  (void)vm;

  if (find_len == 0) {
    return bs_vm_make_string(bs_builtin_store_temp_string(source));
  }

  out[0] = '\0';
  while (*source != '\0' && out_len + 1u < sizeof(out)) {
    if (strncmp(source, find, find_len) == 0) {
      size_t copy_len = replace_len;
      if (out_len + copy_len + 1u >= sizeof(out)) {
        copy_len = (sizeof(out) - out_len) - 1u;
      }
      memcpy(out + out_len, replace, copy_len);
      out_len += copy_len;
      source += find_len;
    } else {
      out[out_len] = *source;
      out_len++;
      source++;
    }
  }
  out[out_len] = '\0';
  return bs_vm_make_string(bs_builtin_store_temp_string(out));
}

static bs_vm_value bs_builtin_variable_global_exists(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char scratch[128];
  const char *name = bs_builtin_arg_to_string(args, argc, 0, scratch, sizeof(scratch));
  int32_t index = bs_builtin_find_global_variable_index_by_name(vm, name);
  if (index < 0) {
    return bs_vm_make_number(0.0);
  }
  return bs_vm_make_number(bs_builtin_global_exists_by_index(vm, index) ? 1.0 : 0.0);
}

static bs_vm_value bs_builtin_variable_global_set(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char scratch[128];
  const char *name = bs_builtin_arg_to_string(args, argc, 0, scratch, sizeof(scratch));
  double value = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  int32_t index = bs_builtin_find_global_variable_index_by_name(vm, name);
  if (index >= 0) {
    (void)bs_builtin_global_set_by_index(vm, index, value);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_variable_global_get(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char scratch[128];
  const char *name = bs_builtin_arg_to_string(args, argc, 0, scratch, sizeof(scratch));
  int32_t index = bs_builtin_find_global_variable_index_by_name(vm, name);
  if (index < 0) {
    return bs_vm_make_number(0.0);
  }
  return bs_vm_make_number(bs_builtin_global_get_by_index(vm, index));
}

static bs_vm_value bs_builtin_ds_map_create(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  bs_ds_map map = {0};
  bs_ds_map *grown = NULL;
  (void)vm;
  (void)args;
  (void)argc;
  if (g_ds_map_count == g_ds_map_capacity) {
    size_t new_capacity = (g_ds_map_capacity == 0) ? 16u : (g_ds_map_capacity * 2u);
    grown = (bs_ds_map *)realloc(g_ds_maps, new_capacity * sizeof(bs_ds_map));
    if (grown == NULL) {
      return bs_vm_make_number(-1.0);
    }
    g_ds_maps = grown;
    g_ds_map_capacity = new_capacity;
  }

  map.id = g_next_ds_map_id++;
  map.entries = NULL;
  map.entry_count = 0;
  map.entry_capacity = 0;
  g_ds_maps[g_ds_map_count] = map;
  g_ds_map_count++;
  return bs_vm_make_number((double)map.id);
}

static bs_vm_value bs_builtin_ds_map_set(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  if (argc >= 3) {
    char scratch[128];
    int32_t map_id = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
    const char *key = bs_builtin_arg_to_string(args, argc, 1, scratch, sizeof(scratch));
    (void)bs_ds_map_set_value(map_id, key, args[2]);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_ds_map_add(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  return bs_builtin_ds_map_set(vm, args, argc);
}

static bs_vm_value bs_builtin_script_execute(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t script_index = -1;
  int32_t code_id = -1;
  const bs_vm_value *script_args = NULL;
  size_t script_argc = 0;
  static int trace_init = 0;
  static bool trace_enabled = false;
  static bool trace_vm_enabled = false;
  if (!trace_init) {
    const char *env = getenv("BS_TRACE_SCRIPT_EXECUTE");
    const char *env_vm = getenv("BS_TRACE_SCRIPT_EXECUTE_VM");
    trace_enabled = (env != NULL && (strcmp(env, "1") == 0 || strcmp(env, "true") == 0));
    trace_vm_enabled =
        (env_vm != NULL && (strcmp(env_vm, "1") == 0 || strcmp(env_vm, "true") == 0));
    trace_init = 1;
  }
  if (vm == NULL || vm->game_data == NULL || argc < 1) {
    return bs_vm_make_number(0.0);
  }

  script_index = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  if (trace_enabled) {
    const char *script_name = NULL;
    if (script_index >= 0 && (size_t)script_index < vm->game_data->script_count) {
      script_name = vm->game_data->scripts[(size_t)script_index].name;
    }
    printf("  [SCRIPT_EXECUTE] argc=%zu script_index=%d name=%s",
           argc,
           script_index,
           script_name != NULL ? script_name : "-");
    for (size_t i = 1; i < argc; i++) {
      if (args[i].type == BS_VM_VALUE_STRING) {
        printf(" arg%zu=\"%s\"", i - 1u, args[i].string != NULL ? args[i].string : "");
      } else {
        printf(" arg%zu=%.3f", i - 1u, args[i].number);
      }
    }
    printf("\n");
  }
  if (script_index < 0 || (size_t)script_index >= vm->game_data->script_count) {
    return bs_vm_make_number(0.0);
  }

  code_id = vm->game_data->scripts[(size_t)script_index].code_id;
  if (code_id >= 0 && (size_t)code_id < vm->game_data->code_entry_count) {
    bs_vm_execute_result result = {0};
    if (argc > 1) {
      script_args = &args[1];
      script_argc = argc - 1u;
    }
    (void)bs_vm_execute_code_with_args(vm,
                                       (size_t)code_id,
                                       script_args,
                                       script_argc,
                                       120000u,
                                       trace_vm_enabled,
                                       &result);
    return result.return_value_value;
  }

  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_ds_map_find_value(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  bs_ds_map_value value = {0};
  char scratch[128];
  int32_t map_id = 0;
  const char *key = NULL;
  static int trace_init = 0;
  static bool trace_enabled = false;
  static int trace_count = 0;
  (void)vm;
  if (!trace_init) {
    const char *env = getenv("BS_TRACE_DS_MAP");
    trace_enabled = (env != NULL && (strcmp(env, "1") == 0 || strcmp(env, "true") == 0));
    trace_init = 1;
  }
  if (argc < 2) {
    return bs_vm_make_number(NAN);
  }
  map_id = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  key = bs_builtin_arg_to_string(args, argc, 1, scratch, sizeof(scratch));
  if (!bs_ds_map_find_value(map_id, key, &value)) {
    if (trace_enabled && trace_count < 200) {
      printf("  [DS_MAP_FIND] map=%d key=\"%s\" -> <undefined>\n", map_id, key);
      trace_count++;
    }
    return bs_vm_make_number(NAN);
  }
  if (value.is_string) {
    if (trace_enabled && trace_count < 200) {
      printf("  [DS_MAP_FIND] map=%d key=\"%s\" -> \"%s\"\n",
             map_id,
             key,
             value.string != NULL ? value.string : "");
      trace_count++;
    }
    return bs_vm_make_string(bs_builtin_store_temp_string(value.string != NULL ? value.string : ""));
  }
  if (trace_enabled && trace_count < 200) {
    printf("  [DS_MAP_FIND] map=%d key=\"%s\" -> %.3f\n", map_id, key, value.number);
    trace_count++;
  }
  return bs_vm_make_number(value.number);
}

static bs_vm_value bs_builtin_is_undefined(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  if (argc < 1) {
    return bs_vm_make_number(1.0);
  }
  if (args[0].type == BS_VM_VALUE_NUMBER && isnan(args[0].number)) {
    return bs_vm_make_number(1.0);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_is_string(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  if (argc < 1) {
    return bs_vm_make_number(0.0);
  }
  return bs_vm_make_number(args[0].type == BS_VM_VALUE_STRING ? 1.0 : 0.0);
}

static bs_vm_value bs_builtin_is_real(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  if (argc < 1) {
    return bs_vm_make_number(0.0);
  }
  if (args[0].type == BS_VM_VALUE_NUMBER && !isnan(args[0].number)) {
    return bs_vm_make_number(1.0);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_is_array(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_typeof_fn(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  if (argc < 1) {
    return bs_vm_make_string("undefined");
  }
  if (args[0].type == BS_VM_VALUE_STRING) {
    return bs_vm_make_string("string");
  }
  if (args[0].type == BS_VM_VALUE_NUMBER && !isnan(args[0].number)) {
    return bs_vm_make_string("number");
  }
  return bs_vm_make_string("undefined");
}

static bs_vm_value bs_builtin_ini_open(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_ini_close(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_ini_read_real(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  return bs_vm_make_number(bs_builtin_arg_to_number(args, argc, 2, 0.0));
}

static bs_vm_value bs_builtin_ini_read_string(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char scratch[128];
  const char *fallback = bs_builtin_arg_to_string(args, argc, 2, scratch, sizeof(scratch));
  (void)vm;
  return bs_vm_make_string(bs_builtin_store_temp_string(fallback));
}

static bs_vm_value bs_builtin_ini_section_exists(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_ini_write_real(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_file_exists(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  (void)args;
  (void)argc;
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_action_move_to(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || vm->current_self_id < 0 || argc < 2) {
    return bs_vm_make_number(0.0);
  }

  (void)bs_game_runner_instance_set_variable(vm->runner,
                                             vm->current_self_id,
                                             -1,
                                             "x",
                                             bs_builtin_arg_to_number(args, argc, 0, 0.0));
  (void)bs_game_runner_instance_set_variable(vm->runner,
                                             vm->current_self_id,
                                             -1,
                                             "y",
                                             bs_builtin_arg_to_number(args, argc, 1, 0.0));
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_action_set_alarm(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  bs_instance *self = NULL;
  int32_t alarm_value = 0;
  int32_t alarm_index = 0;
  if (vm == NULL || vm->runner == NULL || argc < 2 || vm->current_self_id < 0) {
    return bs_vm_make_number(0.0);
  }

  self = bs_game_runner_find_instance_by_id(vm->runner, vm->current_self_id);
  if (self == NULL) {
    return bs_vm_make_number(0.0);
  }

  alarm_value = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  alarm_index = (int32_t)bs_builtin_arg_to_number(args, argc, 1, 0.0);
  if (alarm_index >= 0 && alarm_index < 12) {
    self->alarm[alarm_index] = alarm_value;
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_action_kill_object(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm == NULL || vm->runner == NULL || vm->current_self_id < 0) {
    return bs_vm_make_number(0.0);
  }
  bs_game_runner_destroy_instance(vm->runner, vm->current_self_id);
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_event_inherited(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  bs_instance *self = NULL;
  (void)args;
  (void)argc;
  if (vm == NULL || vm->runner == NULL || vm->current_self_id < 0) {
    return bs_vm_make_number(0.0);
  }
  self = bs_game_runner_find_instance_by_id(vm->runner, vm->current_self_id);
  if (self != NULL) {
    bs_game_runner_fire_event_inherited(vm->runner, self);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_event_user(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  bs_instance *self = bs_builtin_get_self_instance(vm);
  int32_t user_event_num = (int32_t)bs_builtin_arg_to_number(args, argc, 0, 0.0);
  if (vm != NULL && vm->runner != NULL && self != NULL) {
    bs_game_runner_fire_event_for_instance(vm->runner, self, BS_EVENT_OTHER, 10 + user_event_num);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_event_perform(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  bs_instance *self = bs_builtin_get_self_instance(vm);
  int32_t event_type = (int32_t)bs_builtin_arg_to_number(args, argc, 0, 0.0);
  int32_t event_subtype = (int32_t)bs_builtin_arg_to_number(args, argc, 1, 0.0);
  if (vm != NULL && vm->runner != NULL && self != NULL) {
    bs_game_runner_fire_event_for_instance(vm->runner, self, event_type, event_subtype);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_audio_play_sound(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || argc < 3) {
    return bs_vm_make_number(-1.0);
  }
  int32_t sound_index = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  double priority = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  bool loop = bs_builtin_arg_to_number(args, argc, 2, 0.0) > 0.5;
  if (vm->runner->audio.play_sound != NULL) {
    int32_t handle = vm->runner->audio.play_sound(vm->runner->audio.userdata, vm->runner, sound_index, loop, priority);
    return bs_vm_make_number((double)handle);
  }
  return bs_vm_make_number(-1.0);
}

static bs_vm_value bs_builtin_audio_sound_pitch(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || argc < 2) {
    return bs_vm_make_number(0.0);
  }
  int32_t handle = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  double pitch = bs_builtin_arg_to_number(args, argc, 1, 1.0);
  if (vm->runner->audio.set_pitch != NULL) {
    vm->runner->audio.set_pitch(vm->runner->audio.userdata, handle, pitch);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_audio_sound_gain(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || argc < 3) {
    return bs_vm_make_number(0.0);
  }
  int32_t handle = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  double volume = bs_builtin_arg_to_number(args, argc, 1, 1.0);
  double duration_ms = bs_builtin_arg_to_number(args, argc, 2, 0.0);
  if (vm->runner->audio.set_gain != NULL) {
    vm->runner->audio.set_gain(vm->runner->audio.userdata, handle, volume, duration_ms);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_audio_stop_sound(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || argc < 1) {
    return bs_vm_make_number(0.0);
  }
  int32_t handle = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  if (vm->runner->audio.stop_sound != NULL) {
    vm->runner->audio.stop_sound(vm->runner->audio.userdata, handle);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_audio_stop_all(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm != NULL && vm->runner != NULL && vm->runner->audio.stop_all != NULL) {
    vm->runner->audio.stop_all(vm->runner->audio.userdata);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_audio_is_playing(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || argc < 1) {
    return bs_vm_make_number(0.0);
  }
  int32_t handle = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  if (vm->runner->audio.is_playing != NULL) {
    return bs_vm_make_number(vm->runner->audio.is_playing(vm->runner->audio.userdata, handle) ? 1.0 : 0.0);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_audio_pause_sound(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || argc < 1) {
    return bs_vm_make_number(0.0);
  }
  int32_t handle = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  if (vm->runner->audio.pause_sound != NULL) {
    vm->runner->audio.pause_sound(vm->runner->audio.userdata, handle);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_audio_resume_sound(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || argc < 1) {
    return bs_vm_make_number(0.0);
  }
  int32_t handle = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  if (vm->runner->audio.resume_sound != NULL) {
    vm->runner->audio.resume_sound(vm->runner->audio.userdata, handle);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_audio_master_gain(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || argc < 1) {
    return bs_vm_make_number(0.0);
  }
  double volume = bs_builtin_arg_to_number(args, argc, 0, 1.0);
  if (vm->runner->audio.set_master_gain != NULL) {
    vm->runner->audio.set_master_gain(vm->runner->audio.userdata, volume);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_audio_sound_set_track_position(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || argc < 2) {
    return bs_vm_make_number(0.0);
  }
  int32_t handle = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  double position = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  if (vm->runner->audio.set_track_position != NULL) {
    vm->runner->audio.set_track_position(vm->runner->audio.userdata, handle, position);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_audio_sound_get_track_position(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || argc < 1) {
    return bs_vm_make_number(0.0);
  }
  int32_t handle = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  if (vm->runner->audio.get_track_position != NULL) {
    return bs_vm_make_number(vm->runner->audio.get_track_position(vm->runner->audio.userdata, handle));
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_set_color(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm != NULL && vm->runner != NULL && argc > 0) {
    vm->runner->draw_color = bs_builtin_color_to_u24(bs_builtin_arg_to_number(args, argc, 0, 0xFFFFFF));
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_get_color(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm != NULL && vm->runner != NULL) {
    return bs_vm_make_number((double)vm->runner->draw_color);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_set_alpha(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm != NULL && vm->runner != NULL && argc > 0) {
    vm->runner->draw_alpha = bs_builtin_alpha01_to_u8(bs_builtin_arg_to_number(args, argc, 0, 1.0));
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_get_alpha(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm != NULL && vm->runner != NULL) {
    return bs_vm_make_number(vm->runner->draw_alpha / 255.0);
  }
  return bs_vm_make_number(1.0);
}

static bs_vm_value bs_builtin_draw_set_halign(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm != NULL && vm->runner != NULL && argc > 0) {
    vm->runner->draw_halign = (int32_t)bs_builtin_arg_to_number(args, argc, 0, 0.0);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_set_valign(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm != NULL && vm->runner != NULL && argc > 0) {
    vm->runner->draw_valign = (int32_t)bs_builtin_arg_to_number(args, argc, 0, 0.0);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_rectangle(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double x1, y1, x2, y2;
  bool outline;
  
  if (vm == NULL || vm->runner == NULL || argc < 5) {
    return bs_vm_make_number(0.0);
  }
  
  x1 = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  y1 = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  x2 = bs_builtin_arg_to_number(args, argc, 2, 0.0);
  y2 = bs_builtin_arg_to_number(args, argc, 3, 0.0);
  outline = bs_builtin_arg_to_number(args, argc, 4, 0.0) != 0.0;
  
  if (vm->runner->render.draw_rect != NULL) {
    vm->runner->render.draw_rect(vm->runner->render.userdata,
                                 vm->runner,
                                 x1, y1, x2, y2,
                                 outline,
                                 vm->runner->draw_color);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_ossafe_fill_rectangle(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double x1, y1, x2, y2, temp;
  
  if (vm == NULL || vm->runner == NULL || argc < 4) {
    return bs_vm_make_number(0.0);
  }
  /* Get coordinates */
  x1 = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  y1 = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  x2 = bs_builtin_arg_to_number(args, argc, 2, 0.0);
  y2 = bs_builtin_arg_to_number(args, argc, 3, 0.0);
  
  /* Swap if x1 > x2 */
  if (x1 > x2) {
    temp = x1;
    x1 = x2;
    x2 = temp;
  }
  
  /* Swap if y1 > y2 */
  if (y1 > y2) {
    temp = y1;
    y1 = y2;
    y2 = temp;
  }
  
  /* Draw filled rectangle (outline = false) */
  if (vm->runner->render.draw_rect != NULL) {
    vm->runner->render.draw_rect(vm->runner->render.userdata,
                                 vm->runner,
                                 x1, y1, x2, y2,
                                 false,
                                 vm->runner->draw_color);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_self(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  bs_instance *self = NULL;
  int32_t frame = 0;
  (void)args;
  (void)argc;
  if (vm == NULL || vm->runner == NULL || vm->current_self_id < 0) {
    return bs_vm_make_number(0.0);
  }
  self = bs_game_runner_find_instance_by_id(vm->runner, vm->current_self_id);
  if (self == NULL ||
      self->sprite_index < 0 ||
      (vm->runner->render.draw_sprite_ext == NULL && vm->runner->render.draw_sprite == NULL)) {
    return bs_vm_make_number(0.0);
  }
  frame = (self->image_single >= 0.0) ? (int32_t)self->image_single : (int32_t)floor(self->image_index);
  if (vm->runner->render.draw_sprite_ext != NULL) {
    vm->runner->render.draw_sprite_ext(vm->runner->render.userdata,
                                       vm->runner,
                                       self->sprite_index,
                                       frame,
                                       self->x,
                                       self->y,
                                       self->image_xscale,
                                       self->image_yscale,
                                       self->image_angle,
                                       self->image_blend,
                                       self->image_alpha);
  } else {
    vm->runner->render.draw_sprite(vm->runner->render.userdata,
                                   vm->runner,
                                   self->sprite_index,
                                   frame,
                                   self->x,
                                   self->y,
                                   self->image_blend,
                                   self->image_alpha);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_set_font(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm != NULL && vm->runner != NULL && argc > 0) {
    vm->runner->draw_font_index = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_surface_get_width(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm != NULL && vm->runner != NULL && vm->runner->surface_width > 0) {
    return bs_vm_make_number((double)vm->runner->surface_width);
  }
  return bs_vm_make_number(640.0);
}

static bs_vm_value bs_builtin_surface_get_height(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)args;
  (void)argc;
  if (vm != NULL && vm->runner != NULL && vm->runner->surface_height > 0) {
    return bs_vm_make_number((double)vm->runner->surface_height);
  }
  return bs_vm_make_number(480.0);
}

static bs_vm_value bs_builtin_draw_sprite(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || argc < 4) {
    return bs_vm_make_number(0.0);
  }
  if (vm->runner->render.draw_sprite_ext != NULL) {
    vm->runner->render.draw_sprite_ext(vm->runner->render.userdata,
                                       vm->runner,
                                       (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0),
                                       (int32_t)bs_builtin_arg_to_number(args, argc, 1, 0.0),
                                       bs_builtin_arg_to_number(args, argc, 2, 0.0),
                                       bs_builtin_arg_to_number(args, argc, 3, 0.0),
                                       1.0,
                                       1.0,
                                       0.0,
                                       0x00FFFFFF,
                                       1.0);
  } else if (vm->runner->render.draw_sprite != NULL) {
    vm->runner->render.draw_sprite(vm->runner->render.userdata,
                                   vm->runner,
                                   (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0),
                                   (int32_t)bs_builtin_arg_to_number(args, argc, 1, 0.0),
                                   bs_builtin_arg_to_number(args, argc, 2, 0.0),
                                   bs_builtin_arg_to_number(args, argc, 3, 0.0),
                                   0x00FFFFFF,
                                   1.0);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_sprite_ext(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t blend = 0x00FFFFFF;
  double alpha = 1.0;
  if (vm == NULL || vm->runner == NULL || argc < 9) {
    return bs_vm_make_number(0.0);
  }
  blend = bs_builtin_color_to_u24(bs_builtin_arg_to_number(args, argc, 7, 0xFFFFFF));
  alpha = bs_builtin_arg_to_number(args, argc, 8, 1.0);
  if (alpha < 0.0) {
    alpha = 0.0;
  }
  if (alpha > 1.0) {
    alpha = 1.0;
  }
  if (vm->runner->render.draw_sprite_ext != NULL) {
    vm->runner->render.draw_sprite_ext(vm->runner->render.userdata,
                                       vm->runner,
                                       (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0),
                                       (int32_t)bs_builtin_arg_to_number(args, argc, 1, 0.0),
                                       bs_builtin_arg_to_number(args, argc, 2, 0.0),
                                       bs_builtin_arg_to_number(args, argc, 3, 0.0),
                                       bs_builtin_arg_to_number(args, argc, 4, 1.0),
                                       bs_builtin_arg_to_number(args, argc, 5, 1.0),
                                       bs_builtin_arg_to_number(args, argc, 6, 0.0),
                                       blend,
                                       alpha);
  } else if (vm->runner->render.draw_sprite != NULL) {
    vm->runner->render.draw_sprite(vm->runner->render.userdata,
                                   vm->runner,
                                   (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0),
                                   (int32_t)bs_builtin_arg_to_number(args, argc, 1, 0.0),
                                   bs_builtin_arg_to_number(args, argc, 2, 0.0),
                                   bs_builtin_arg_to_number(args, argc, 3, 0.0),
                                   blend,
                                   alpha);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_sprite_part(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  if (vm == NULL || vm->runner == NULL || argc < 8) {
    return bs_vm_make_number(0.0);
  }
  if (vm->runner->render.draw_sprite_part_ext != NULL) {
    vm->runner->render.draw_sprite_part_ext(vm->runner->render.userdata,
                                            vm->runner,
                                            (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0),
                                            (int32_t)bs_builtin_arg_to_number(args, argc, 1, 0.0),
                                            (int32_t)bs_builtin_arg_to_number(args, argc, 2, 0.0),
                                            (int32_t)bs_builtin_arg_to_number(args, argc, 3, 0.0),
                                            (int32_t)bs_builtin_arg_to_number(args, argc, 4, 0.0),
                                            (int32_t)bs_builtin_arg_to_number(args, argc, 5, 0.0),
                                            bs_builtin_arg_to_number(args, argc, 6, 0.0),
                                            bs_builtin_arg_to_number(args, argc, 7, 0.0),
                                            1.0,
                                            1.0,
                                            0x00FFFFFF,
                                            1.0);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_sprite_part_ext(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t blend = 0x00FFFFFF;
  double alpha = 1.0;
  if (vm == NULL || vm->runner == NULL || argc < 12) {
    return bs_vm_make_number(0.0);
  }
  blend = bs_builtin_color_to_u24(bs_builtin_arg_to_number(args, argc, 10, 0xFFFFFF));
  alpha = bs_builtin_arg_to_number(args, argc, 11, 1.0);
  if (alpha < 0.0) {
    alpha = 0.0;
  }
  if (alpha > 1.0) {
    alpha = 1.0;
  }
  if (vm->runner->render.draw_sprite_part_ext != NULL) {
    vm->runner->render.draw_sprite_part_ext(vm->runner->render.userdata,
                                            vm->runner,
                                            (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0),
                                            (int32_t)bs_builtin_arg_to_number(args, argc, 1, 0.0),
                                            (int32_t)bs_builtin_arg_to_number(args, argc, 2, 0.0),
                                            (int32_t)bs_builtin_arg_to_number(args, argc, 3, 0.0),
                                            (int32_t)bs_builtin_arg_to_number(args, argc, 4, 0.0),
                                            (int32_t)bs_builtin_arg_to_number(args, argc, 5, 0.0),
                                            bs_builtin_arg_to_number(args, argc, 6, 0.0),
                                            bs_builtin_arg_to_number(args, argc, 7, 0.0),
                                            bs_builtin_arg_to_number(args, argc, 8, 1.0),
                                            bs_builtin_arg_to_number(args, argc, 9, 1.0),
                                            blend,
                                            alpha);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_text(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char text_scratch[512];
  const char *text = bs_builtin_arg_to_string(args, argc, 2, text_scratch, sizeof(text_scratch));
  if (vm == NULL || vm->runner == NULL || argc < 3) {
    return bs_vm_make_number(0.0);
  }
  if (vm->runner->render.draw_text != NULL) {
    vm->runner->render.draw_text(vm->runner->render.userdata,
                                 vm->runner,
                                 text,
                                 bs_builtin_arg_to_number(args, argc, 0, 0.0),
                                 bs_builtin_arg_to_number(args, argc, 1, 0.0),
                                 vm->runner->draw_font_index,
                                 vm->runner->draw_color,
                                 1.0,
                                 1.0);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_text_ext(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  return bs_builtin_draw_text(vm, args, argc);
}

static bs_vm_value bs_builtin_draw_text_transformed(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  char text_scratch[512];
  const char *text = bs_builtin_arg_to_string(args, argc, 2, text_scratch, sizeof(text_scratch));
  if (vm == NULL || vm->runner == NULL || argc < 6) {
    return bs_vm_make_number(0.0);
  }
  if (vm->runner->render.draw_text != NULL) {
    vm->runner->render.draw_text(vm->runner->render.userdata,
                                 vm->runner,
                                 text,
                                 bs_builtin_arg_to_number(args, argc, 0, 0.0),
                                 bs_builtin_arg_to_number(args, argc, 1, 0.0),
                                 vm->runner->draw_font_index,
                                 vm->runner->draw_color,
                                 bs_builtin_arg_to_number(args, argc, 3, 1.0),
                                 bs_builtin_arg_to_number(args, argc, 4, 1.0));
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_background(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t background_index = -1;
  int32_t tpag_index = -1;
  if (vm == NULL || vm->runner == NULL || vm->game_data == NULL || argc < 3) {
    return bs_vm_make_number(0.0);
  }
  if (vm->runner->render.draw_background == NULL) {
    return bs_vm_make_number(0.0);
  }
  background_index = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  if (background_index < 0 || (size_t)background_index >= vm->game_data->background_count) {
    return bs_vm_make_number(0.0);
  }
  tpag_index = vm->game_data->backgrounds[(size_t)background_index].tpag_index;
  if (tpag_index < 0) {
    return bs_vm_make_number(0.0);
  }
  vm->runner->render.draw_background(vm->runner->render.userdata,
                                     vm->runner,
                                     tpag_index,
                                     (int32_t)bs_builtin_arg_to_number(args, argc, 1, 0.0),
                                     (int32_t)bs_builtin_arg_to_number(args, argc, 2, 0.0),
                                     false,
                                     false);
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_draw_background_ext(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  return bs_builtin_draw_background(vm, args, argc);
}

static bs_vm_value bs_builtin_keyboard_check(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t key = 0;
  bool result = false;
  if (vm == NULL || vm->runner == NULL || argc < 1) {
    return bs_vm_make_number(0.0);
  }
  key = (int32_t)bs_builtin_arg_to_number(args, argc, 0, 0.0);
  if (key == 1) {
    result = bs_builtin_any_key_state(vm->runner->keys_held);
  } else if (key == 0) {
    result = !bs_builtin_any_key_state(vm->runner->keys_held);
  } else if (key >= 0 && key < 256) {
    result = vm->runner->keys_held[key];
  } else {
    result = false;
  }
  return bs_vm_make_number(result ? 1.0 : 0.0);
}

static bs_vm_value bs_builtin_keyboard_check_pressed(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t key = 0;
  bool result = false;
  if (vm == NULL || vm->runner == NULL || argc < 1) {
    return bs_vm_make_number(0.0);
  }
  key = (int32_t)bs_builtin_arg_to_number(args, argc, 0, 0.0);
  if (key == 1) {
    result = bs_builtin_any_key_state(vm->runner->keys_pressed);
  } else if (key == 0) {
    result = !bs_builtin_any_key_state(vm->runner->keys_pressed);
  } else if (key >= 0 && key < 256) {
    result = vm->runner->keys_pressed[key];
  } else {
    result = false;
  }
  return bs_vm_make_number(result ? 1.0 : 0.0);
}

static bs_vm_value bs_builtin_keyboard_check_released(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t key = 0;
  bool result = false;
  if (vm == NULL || vm->runner == NULL || argc < 1) {
    return bs_vm_make_number(0.0);
  }
  key = (int32_t)bs_builtin_arg_to_number(args, argc, 0, 0.0);
  if (key == 1) {
    result = bs_builtin_any_key_state(vm->runner->keys_released);
  } else if (key == 0) {
    result = !bs_builtin_any_key_state(vm->runner->keys_released);
  } else if (key >= 0 && key < 256) {
    result = vm->runner->keys_released[key];
  } else {
    result = false;
  }
  return bs_vm_make_number(result ? 1.0 : 0.0);
}

static bs_vm_value bs_builtin_keyboard_check_direct(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  return bs_builtin_keyboard_check(vm, args, argc);
}

static bs_vm_value bs_builtin_collision_point(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double px = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double py = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  int32_t target = (int32_t)bs_builtin_arg_to_number(args, argc, 2, -1.0);
  bool precise = bs_builtin_arg_to_number(args, argc, 3, 0.0) != 0.0;
  bool notme = bs_builtin_arg_to_number(args, argc, 4, 0.0) != 0.0;
  int32_t self_id = -1;
  (void)precise;
  if (vm == NULL || vm->runner == NULL || argc < 5) {
    return bs_vm_make_number(-4.0);
  }
  self_id = vm->current_self_id;

  for (size_t i = 0; i < vm->runner->instance_count; i++) {
    bs_instance *inst = &vm->runner->instances[i];
    bs_bbox bbox = {0};
    if (!bs_builtin_instance_matches_target(vm->runner, inst, target)) {
      continue;
    }
    if (notme && inst->id == self_id) {
      continue;
    }
    if (!bs_game_runner_compute_instance_bbox(vm->runner, inst, &bbox)) {
      continue;
    }
    if (px >= bbox.left && px < bbox.right && py >= bbox.top && py < bbox.bottom) {
      return bs_vm_make_number((double)inst->id);
    }
  }

  return bs_vm_make_number(-4.0);
}

static bs_vm_value bs_builtin_collision_rectangle(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double x1 = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double y1 = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  double x2 = bs_builtin_arg_to_number(args, argc, 2, 0.0);
  double y2 = bs_builtin_arg_to_number(args, argc, 3, 0.0);
  int32_t target = (int32_t)bs_builtin_arg_to_number(args, argc, 4, -1.0);
  bool precise = bs_builtin_arg_to_number(args, argc, 5, 0.0) != 0.0;
  bool notme = bs_builtin_arg_to_number(args, argc, 6, 0.0) != 0.0;
  int32_t self_id = -1;
  double ql = fmin(x1, x2);
  double qr = fmax(x1, x2);
  double qt = fmin(y1, y2);
  double qb = fmax(y1, y2);
  (void)precise;
  if (vm == NULL || vm->runner == NULL || argc < 7) {
    return bs_vm_make_number(-4.0);
  }
  self_id = vm->current_self_id;

  for (size_t i = 0; i < vm->runner->instance_count; i++) {
    bs_instance *inst = &vm->runner->instances[i];
    bs_bbox bbox = {0};
    if (!bs_builtin_instance_matches_target(vm->runner, inst, target)) {
      continue;
    }
    if (notme && inst->id == self_id) {
      continue;
    }
    if (!bs_game_runner_compute_instance_bbox(vm->runner, inst, &bbox)) {
      continue;
    }
    if (ql < bbox.right && qr >= bbox.left && qt < bbox.bottom && qb >= bbox.top) {
      return bs_vm_make_number((double)inst->id);
    }
  }

  return bs_vm_make_number(-4.0);
}

static bs_vm_value bs_builtin_collision_circle(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double cx = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double cy = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  double radius = bs_builtin_arg_to_number(args, argc, 2, 0.0);
  int32_t target = (int32_t)bs_builtin_arg_to_number(args, argc, 3, -1.0);
  bool precise = bs_builtin_arg_to_number(args, argc, 4, 0.0) != 0.0;
  bool notme = bs_builtin_arg_to_number(args, argc, 5, 0.0) != 0.0;
  int32_t self_id = -1;
  double radius_sq = radius * radius;
  (void)precise;
  if (vm == NULL || vm->runner == NULL || argc < 6) {
    return bs_vm_make_number(-4.0);
  }
  self_id = vm->current_self_id;

  for (size_t i = 0; i < vm->runner->instance_count; i++) {
    bs_instance *inst = &vm->runner->instances[i];
    bs_bbox bbox = {0};
    if (!bs_builtin_instance_matches_target(vm->runner, inst, target)) {
      continue;
    }
    if (notme && inst->id == self_id) {
      continue;
    }
    if (!bs_game_runner_compute_instance_bbox(vm->runner, inst, &bbox)) {
      continue;
    }

    {
      double nearest_x = fmin(fmax(cx, bbox.left), bbox.right);
      double nearest_y = fmin(fmax(cy, bbox.top), bbox.bottom);
      double dx = cx - nearest_x;
      double dy = cy - nearest_y;
      if ((dx * dx) + (dy * dy) <= radius_sq) {
        return bs_vm_make_number((double)inst->id);
      }
    }
  }

  return bs_vm_make_number(-4.0);
}

static bs_vm_value bs_builtin_collision_line(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double x1 = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double y1 = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  double x2 = bs_builtin_arg_to_number(args, argc, 2, 0.0);
  double y2 = bs_builtin_arg_to_number(args, argc, 3, 0.0);
  int32_t target = (int32_t)bs_builtin_arg_to_number(args, argc, 4, -1.0);
  bool precise = bs_builtin_arg_to_number(args, argc, 5, 0.0) != 0.0;
  bool notme = bs_builtin_arg_to_number(args, argc, 6, 0.0) != 0.0;
  double dx = x2 - x1;
  double dy = y2 - y1;
  int32_t self_id = -1;
  (void)precise;
  if (vm == NULL || vm->runner == NULL || argc < 7) {
    return bs_vm_make_number(-4.0);
  }
  self_id = vm->current_self_id;

  for (size_t i = 0; i < vm->runner->instance_count; i++) {
    bs_instance *inst = &vm->runner->instances[i];
    bs_bbox bbox = {0};
    double t_min = 0.0;
    double t_max = 1.0;
    double edges[4];
    double sides[4];
    bool hit = true;
    if (!bs_builtin_instance_matches_target(vm->runner, inst, target)) {
      continue;
    }
    if (notme && inst->id == self_id) {
      continue;
    }
    if (!bs_game_runner_compute_instance_bbox(vm->runner, inst, &bbox)) {
      continue;
    }

    edges[0] = -dx;
    edges[1] = dx;
    edges[2] = -dy;
    edges[3] = dy;
    sides[0] = x1 - bbox.left;
    sides[1] = bbox.right - x1;
    sides[2] = y1 - bbox.top;
    sides[3] = bbox.bottom - y1;
    for (size_t e = 0; e < 4; e++) {
      double p = edges[e];
      double q = sides[e];
      if (p == 0.0) {
        if (q < 0.0) {
          hit = false;
          break;
        }
      } else {
        double t = q / p;
        if (p < 0.0) {
          if (t > t_min) {
            t_min = t;
          }
        } else if (t < t_max) {
          t_max = t;
        }
        if (t_min > t_max) {
          hit = false;
          break;
        }
      }
    }

    if (hit) {
      return bs_vm_make_number((double)inst->id);
    }
  }

  return bs_vm_make_number(-4.0);
}

static bs_vm_value bs_builtin_abs_fn(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  return bs_vm_make_number(fabs(bs_builtin_arg_to_number(args, argc, 0, 0.0)));
}

static bs_vm_value bs_builtin_floor_fn(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  return bs_vm_make_number(floor(bs_builtin_arg_to_number(args, argc, 0, 0.0)));
}

static bs_vm_value bs_builtin_ceil_fn(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  return bs_vm_make_number(ceil(bs_builtin_arg_to_number(args, argc, 0, 0.0)));
}

static bs_vm_value bs_builtin_sign(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double value = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  (void)vm;
  if (value > 0.0) {
    return bs_vm_make_number(1.0);
  }
  if (value < 0.0) {
    return bs_vm_make_number(-1.0);
  }
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_clamp(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double value = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double low = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  double high = bs_builtin_arg_to_number(args, argc, 2, 0.0);
  (void)vm;
  if (value < low) {
    value = low;
  }
  if (value > high) {
    value = high;
  }
  return bs_vm_make_number(value);
}

static bs_vm_value bs_builtin_sqrt_fn(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  (void)vm;
  return bs_vm_make_number(sqrt(bs_builtin_arg_to_number(args, argc, 0, 0.0)));
}

static bs_vm_value bs_builtin_power(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double base = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double exponent = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  (void)vm;
  return bs_vm_make_number(pow(base, exponent));
}

static bs_vm_value bs_builtin_sin_fn(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  const double pi = 3.14159265358979323846;
  double degrees = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  (void)vm;
  return bs_vm_make_number(sin(degrees * (pi / 180.0)));
}

static bs_vm_value bs_builtin_cos_fn(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  const double pi = 3.14159265358979323846;
  double degrees = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  (void)vm;
  return bs_vm_make_number(cos(degrees * (pi / 180.0)));
}

static bs_vm_value bs_builtin_degtorad(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  const double pi = 3.14159265358979323846;
  double degrees = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  (void)vm;
  return bs_vm_make_number(degrees * (pi / 180.0));
}

static bs_vm_value bs_builtin_radtodeg(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  const double pi = 3.14159265358979323846;
  double radians = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  (void)vm;
  return bs_vm_make_number(radians * (180.0 / pi));
}

static bs_vm_value bs_builtin_point_direction(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  const double pi = 3.14159265358979323846;
  double x1 = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double y1 = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  double x2 = bs_builtin_arg_to_number(args, argc, 2, 0.0);
  double y2 = bs_builtin_arg_to_number(args, argc, 3, 0.0);
  double dx = x2 - x1;
  double dy = y2 - y1;
  double dir = fmod((atan2(-dy, dx) * (180.0 / pi)) + 360.0, 360.0);
  (void)vm;
  return bs_vm_make_number(dir);
}

static bs_vm_value bs_builtin_point_distance(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double x1 = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double y1 = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  double x2 = bs_builtin_arg_to_number(args, argc, 2, 0.0);
  double y2 = bs_builtin_arg_to_number(args, argc, 3, 0.0);
  double dx = x2 - x1;
  double dy = y2 - y1;
  (void)vm;
  return bs_vm_make_number(sqrt((dx * dx) + (dy * dy)));
}

static bs_vm_value bs_builtin_lengthdir_x(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  const double pi = 3.14159265358979323846;
  double length = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double dir = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  (void)vm;
  return bs_vm_make_number(length * cos(dir * (pi / 180.0)));
}

static bs_vm_value bs_builtin_lengthdir_y(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  const double pi = 3.14159265358979323846;
  double length = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double dir = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  (void)vm;
  return bs_vm_make_number(-length * sin(dir * (pi / 180.0)));
}

static bs_vm_value bs_builtin_distance_to_point(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double target_x = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double target_y = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  bs_instance *self = bs_builtin_get_self_instance(vm);
  double dx = 0.0;
  double dy = 0.0;
  
  if (self != NULL) {
    dx = target_x - self->x;
    dy = target_y - self->y;
    return bs_vm_make_number(sqrt((dx * dx) + (dy * dy)));
  }
  
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_move_towards_point(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double target_x = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double target_y = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  double speed = bs_builtin_arg_to_number(args, argc, 2, 0.0);
  bs_instance *self = bs_builtin_get_self_instance(vm);
  const double pi = 3.14159265358979323846;
  
  if (self != NULL) {
    double dx = target_x - self->x;
    double dy = target_y - self->y;
    double distance = sqrt((dx * dx) + (dy * dy));
    
    if (distance > 0.0) {
      double direction = fmod((atan2(-dy, dx) * (180.0 / pi)) + 360.0, 360.0);
      self->direction = direction;
      self->speed = (distance < speed) ? distance : speed;
      self->hspeed = self->speed * cos(direction * (pi / 180.0));
      self->vspeed = -self->speed * sin(direction * (pi / 180.0));
    }
  }
  
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_action_move(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  /* action_move(directions_string, speed)
   * directions_string is a 9-char string of 0s and 1s representing directions:
   * 0=upleft, 1=up, 2=upright, 3=left, 4=none, 5=right, 6=downleft, 7=down, 8=downright
   * Example: "000001000" means move right only
   */
  const char *directions_str = NULL;
  double speed = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  bs_instance *self = bs_builtin_get_self_instance(vm);
  const double pi = 3.14159265358979323846;
  int total_dirs = 0;
  double total_hspeed = 0.0;
  double total_vspeed = 0.0;
  
  if (argc < 2 || args[0].type != BS_VM_VALUE_STRING || self == NULL) {
    return bs_vm_make_number(0.0);
  }
  
  directions_str = args[0].string;
  if (directions_str == NULL || strlen(directions_str) < 9) {
    return bs_vm_make_number(0.0);
  }
  
  /* Check each direction and accumulate velocity */
  if (directions_str[0] != '0') { /* up-left */
    total_hspeed += -cos(45.0 * (pi / 180.0)) * speed;
    total_vspeed += -sin(45.0 * (pi / 180.0)) * speed;
    total_dirs++;
  }
  if (directions_str[1] != '0') { /* up */
    total_vspeed += -speed;
    total_dirs++;
  }
  if (directions_str[2] != '0') { /* up-right */
    total_hspeed += cos(45.0 * (pi / 180.0)) * speed;
    total_vspeed += -sin(45.0 * (pi / 180.0)) * speed;
    total_dirs++;
  }
  if (directions_str[3] != '0') { /* left */
    total_hspeed += -speed;
    total_dirs++;
  }
  /* directions_str[4] is center/no movement */
  if (directions_str[5] != '0') { /* right */
    total_hspeed += speed;
    total_dirs++;
  }
  if (directions_str[6] != '0') { /* down-left */
    total_hspeed += -cos(45.0 * (pi / 180.0)) * speed;
    total_vspeed += sin(45.0 * (pi / 180.0)) * speed;
    total_dirs++;
  }
  if (directions_str[7] != '0') { /* down */
    total_vspeed += speed;
    total_dirs++;
  }
  if (directions_str[8] != '0') { /* down-right */
    total_hspeed += cos(45.0 * (pi / 180.0)) * speed;
    total_vspeed += sin(45.0 * (pi / 180.0)) * speed;
    total_dirs++;
  }
  
  /* Set instance velocity */
  if (total_dirs > 0) {
    self->hspeed = total_hspeed;
    self->vspeed = total_vspeed;
    self->speed = sqrt(total_hspeed * total_hspeed + total_vspeed * total_vspeed);
    if (self->speed > 0.0) {
      self->direction = atan2(-total_vspeed, total_hspeed) * (180.0 / pi);
      if (self->direction < 0.0) {
        self->direction += 360.0;
      }
    }
  }
  
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_action_set_friction(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double friction = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  bs_instance *self = bs_builtin_get_self_instance(vm);
  
  if (self != NULL) {
    self->friction = friction;
  }
  
  return bs_vm_make_number(0.0);
}

static bs_vm_value bs_builtin_lerp(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double a = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double b = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  double t = bs_builtin_arg_to_number(args, argc, 2, 0.0);
  (void)vm;
  return bs_vm_make_number(a + (b - a) * t);
}

static bs_vm_value bs_builtin_choose(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  size_t index = 0;
  (void)vm;
  if (argc == 0) {
    return bs_vm_make_number(0.0);
  }
  index = (size_t)(bs_builtin_rand01() * (double)argc);
  if (index >= argc) {
    index = argc - 1u;
  }
  return args[index];
}

static bs_vm_value bs_builtin_random_range(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double lo = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double hi = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  (void)vm;
  return bs_vm_make_number(lo + (bs_builtin_rand01() * (hi - lo)));
}

static bs_vm_value bs_builtin_irandom_range(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t lo = (int32_t)bs_builtin_arg_to_number(args, argc, 0, 0.0);
  int32_t hi = (int32_t)bs_builtin_arg_to_number(args, argc, 1, 0.0);
  (void)vm;
  if (hi >= lo) {
    int32_t span = hi - lo + 1;
    int32_t n = lo + (int32_t)(bs_builtin_rand01() * (double)span);
    if (n > hi) {
      n = hi;
    }
    return bs_vm_make_number((double)n);
  }
  return bs_vm_make_number((double)lo);
}

static bs_vm_value bs_builtin_room_exists(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t room_index = 0;
  if (vm == NULL || vm->game_data == NULL || argc < 1) {
    return bs_vm_make_number(0.0);
  }
  room_index = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  return bs_vm_make_number((room_index >= 0 && (size_t)room_index < vm->game_data->room_count) ? 1.0 : 0.0);
}

static bs_vm_value bs_builtin_room_next(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t room_index = 0;
  if (vm == NULL || vm->game_data == NULL || argc < 1) {
    return bs_vm_make_number(-1.0);
  }
  room_index = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  for (size_t i = 0; i + 1 < vm->game_data->gen8.room_order_count; i++) {
    if ((int32_t)vm->game_data->gen8.room_order[i] == room_index) {
      return bs_vm_make_number((double)vm->game_data->gen8.room_order[i + 1]);
    }
  }
  return bs_vm_make_number(-1.0);
}

static bs_vm_value bs_builtin_room_previous(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t room_index = 0;
  if (vm == NULL || vm->game_data == NULL || argc < 1) {
    return bs_vm_make_number(-1.0);
  }
  room_index = (int32_t)bs_builtin_arg_to_number(args, argc, 0, -1.0);
  for (size_t i = 1; i < vm->game_data->gen8.room_order_count; i++) {
    if ((int32_t)vm->game_data->gen8.room_order[i] == room_index) {
      return bs_vm_make_number((double)vm->game_data->gen8.room_order[i - 1]);
    }
  }
  return bs_vm_make_number(-1.0);
}

static bs_vm_value bs_builtin_random(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double max_value = bs_builtin_arg_to_number(args, argc, 0, 1.0);
  (void)vm;
  return bs_vm_make_number(bs_builtin_rand01() * max_value);
}

static bs_vm_value bs_builtin_irandom(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  int32_t max_value = (int32_t)bs_builtin_arg_to_number(args, argc, 0, 0.0);
  (void)vm;
  if (max_value <= 0) {
    return bs_vm_make_number(0.0);
  }
  return bs_vm_make_number((double)((int32_t)(bs_builtin_rand01() * (max_value + 1))));
}

static bs_vm_value bs_builtin_min(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double a = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double b = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  (void)vm;
  return bs_vm_make_number(a < b ? a : b);
}

static bs_vm_value bs_builtin_max(bs_vm *vm, const bs_vm_value *args, size_t argc) {
  double a = bs_builtin_arg_to_number(args, argc, 0, 0.0);
  double b = bs_builtin_arg_to_number(args, argc, 1, 0.0);
  (void)vm;
  return bs_vm_make_number(a > b ? a : b);
}

void bs_register_builtins(bs_vm *vm) {
  if (vm == NULL) {
    return;
  }

  (void)bs_vm_register_builtin(vm, "show_debug_message", bs_builtin_show_debug_message);
  (void)bs_vm_register_builtin(vm, "room_goto", bs_builtin_room_goto);
  (void)bs_vm_register_builtin(vm, "room_goto_next", bs_builtin_room_goto_next);
  (void)bs_vm_register_builtin(vm, "room_goto_previous", bs_builtin_room_goto_previous);
  (void)bs_vm_register_builtin(vm, "instance_create", bs_builtin_instance_create);
  (void)bs_vm_register_builtin(vm, "instance_destroy", bs_builtin_instance_destroy);
  (void)bs_vm_register_builtin(vm, "instance_exists", bs_builtin_instance_exists);
  (void)bs_vm_register_builtin(vm, "instance_number", bs_builtin_instance_number);
  (void)bs_vm_register_builtin(vm, "instance_find", bs_builtin_instance_find);
  (void)bs_vm_register_builtin(vm, "path_start", bs_builtin_path_start);
  (void)bs_vm_register_builtin(vm, "path_end", bs_builtin_path_end);

  (void)bs_vm_register_builtin(vm, "os_get_language", bs_builtin_os_get_language);
  (void)bs_vm_register_builtin(vm, "os_get_region", bs_builtin_os_get_region);
  (void)bs_vm_register_builtin(vm, "randomize", bs_builtin_randomize);
  (void)bs_vm_register_builtin(vm, "random_set_seed", bs_builtin_random_set_seed);
  (void)bs_vm_register_builtin(vm, "game_end", bs_builtin_game_end);
  (void)bs_vm_register_builtin(vm, "game_restart", bs_builtin_game_restart);
  (void)bs_vm_register_builtin(vm, "show_message", bs_builtin_show_message);
  (void)bs_vm_register_builtin(vm, "audio_channel_num", bs_builtin_audio_channel_num);
  (void)bs_vm_register_builtin(vm, "steam_initialised", bs_builtin_steam_initialised);
  (void)bs_vm_register_builtin(vm, "steam_stats_ready", bs_builtin_steam_stats_ready);
  (void)bs_vm_register_builtin(vm, "joystick_exists", bs_builtin_joystick_exists);
  (void)bs_vm_register_builtin(vm, "gamepad_get_device_count", bs_builtin_gamepad_get_device_count);
  (void)bs_vm_register_builtin(vm, "gamepad_is_connected", bs_builtin_gamepad_is_connected);
  (void)bs_vm_register_builtin(vm, "sprite_prefetch", bs_builtin_sprite_prefetch);
  (void)bs_vm_register_builtin(vm, "window_set_caption", bs_builtin_window_set_caption);
  (void)bs_vm_register_builtin(vm, "window_get_caption", bs_builtin_window_get_caption);
  (void)bs_vm_register_builtin(vm, "window_set_fullscreen", bs_builtin_window_set_fullscreen);
  (void)bs_vm_register_builtin(vm, "window_get_fullscreen", bs_builtin_window_get_fullscreen);
  (void)bs_vm_register_builtin(vm, "window_get_width", bs_builtin_window_get_width);
  (void)bs_vm_register_builtin(vm, "window_get_height", bs_builtin_window_get_height);
  (void)bs_vm_register_builtin(vm, "display_get_width", bs_builtin_display_get_width);
  (void)bs_vm_register_builtin(vm, "display_get_height", bs_builtin_display_get_height);
  (void)bs_vm_register_builtin(vm, "display_set_gui_size", bs_builtin_display_set_gui_size);
  (void)bs_vm_register_builtin(vm, "view_set_visible", bs_builtin_view_set_visible);
  (void)bs_vm_register_builtin(vm, "application_surface_enable", bs_builtin_application_surface_enable);
  (void)bs_vm_register_builtin(vm, "application_surface_draw_enable", bs_builtin_application_surface_draw_enable);
  (void)bs_vm_register_builtin(vm, "current_time", bs_builtin_current_time_fn);
  (void)bs_vm_register_builtin(vm, "date_current_datetime", bs_builtin_date_current_datetime);
  (void)bs_vm_register_builtin(vm, "get_timer", bs_builtin_get_timer);
  (void)bs_vm_register_builtin(vm, "environment_get_variable", bs_builtin_environment_get_variable);
  (void)bs_vm_register_builtin(vm, "parameter_count", bs_builtin_parameter_count);
  (void)bs_vm_register_builtin(vm, "parameter_string", bs_builtin_parameter_string);
  (void)bs_vm_register_builtin(vm, "real", bs_builtin_real);
  (void)bs_vm_register_builtin(vm, "string", bs_builtin_string_cast);
  (void)bs_vm_register_builtin(vm, "chr", bs_builtin_chr);
  (void)bs_vm_register_builtin(vm, "ord", bs_builtin_ord);
  (void)bs_vm_register_builtin(vm, "ansi_char", bs_builtin_ansi_char);
  (void)bs_vm_register_builtin(vm, "round", bs_builtin_round);
  (void)bs_vm_register_builtin(vm, "abs", bs_builtin_abs_fn);
  (void)bs_vm_register_builtin(vm, "floor", bs_builtin_floor_fn);
  (void)bs_vm_register_builtin(vm, "ceil", bs_builtin_ceil_fn);
  (void)bs_vm_register_builtin(vm, "sign", bs_builtin_sign);
  (void)bs_vm_register_builtin(vm, "clamp", bs_builtin_clamp);
  (void)bs_vm_register_builtin(vm, "sqrt", bs_builtin_sqrt_fn);
  (void)bs_vm_register_builtin(vm, "power", bs_builtin_power);
  (void)bs_vm_register_builtin(vm, "sin", bs_builtin_sin_fn);
  (void)bs_vm_register_builtin(vm, "cos", bs_builtin_cos_fn);
  (void)bs_vm_register_builtin(vm, "degtorad", bs_builtin_degtorad);
  (void)bs_vm_register_builtin(vm, "radtodeg", bs_builtin_radtodeg);
  (void)bs_vm_register_builtin(vm, "point_direction", bs_builtin_point_direction);
  (void)bs_vm_register_builtin(vm, "point_distance", bs_builtin_point_distance);
  (void)bs_vm_register_builtin(vm, "distance_to_point", bs_builtin_distance_to_point);
  (void)bs_vm_register_builtin(vm, "move_towards_point", bs_builtin_move_towards_point);
  (void)bs_vm_register_builtin(vm, "action_move", bs_builtin_action_move);
  (void)bs_vm_register_builtin(vm, "action_set_friction", bs_builtin_action_set_friction);
  (void)bs_vm_register_builtin(vm, "lengthdir_x", bs_builtin_lengthdir_x);
  (void)bs_vm_register_builtin(vm, "lengthdir_y", bs_builtin_lengthdir_y);
  (void)bs_vm_register_builtin(vm, "lerp", bs_builtin_lerp);
  (void)bs_vm_register_builtin(vm, "choose", bs_builtin_choose);
  (void)bs_vm_register_builtin(vm, "random_range", bs_builtin_random_range);
  (void)bs_vm_register_builtin(vm, "irandom_range", bs_builtin_irandom_range);
  (void)bs_vm_register_builtin(vm, "string_lower", bs_builtin_string_lower);
  (void)bs_vm_register_builtin(vm, "string_upper", bs_builtin_string_upper);
  (void)bs_vm_register_builtin(vm, "string_length", bs_builtin_string_length);
  (void)bs_vm_register_builtin(vm, "string_pos", bs_builtin_string_pos);
  (void)bs_vm_register_builtin(vm, "string_width", bs_builtin_string_width);
  (void)bs_vm_register_builtin(vm, "string_height", bs_builtin_string_height);
  (void)bs_vm_register_builtin(vm, "string_char_at", bs_builtin_string_char_at);
  (void)bs_vm_register_builtin(vm, "string_copy", bs_builtin_string_copy);
  (void)bs_vm_register_builtin(vm, "string_replace_all", bs_builtin_string_replace_all);
  (void)bs_vm_register_builtin(vm, "variable_global_exists", bs_builtin_variable_global_exists);
  (void)bs_vm_register_builtin(vm, "variable_global_get", bs_builtin_variable_global_get);
  (void)bs_vm_register_builtin(vm, "variable_global_set", bs_builtin_variable_global_set);
  (void)bs_vm_register_builtin(vm, "ds_map_create", bs_builtin_ds_map_create);
  (void)bs_vm_register_builtin(vm, "ds_map_set", bs_builtin_ds_map_set);
  (void)bs_vm_register_builtin(vm, "ds_map_add", bs_builtin_ds_map_add);
  (void)bs_vm_register_builtin(vm, "ds_map_find_value", bs_builtin_ds_map_find_value);
  (void)bs_vm_register_builtin(vm, "script_execute", bs_builtin_script_execute);
  (void)bs_vm_register_builtin(vm, "is_undefined", bs_builtin_is_undefined);
  (void)bs_vm_register_builtin(vm, "is_string", bs_builtin_is_string);
  (void)bs_vm_register_builtin(vm, "is_real", bs_builtin_is_real);
  (void)bs_vm_register_builtin(vm, "is_array", bs_builtin_is_array);
  (void)bs_vm_register_builtin(vm, "typeof", bs_builtin_typeof_fn);
  (void)bs_vm_register_builtin(vm, "ini_open", bs_builtin_ini_open);
  (void)bs_vm_register_builtin(vm, "ini_close", bs_builtin_ini_close);
  (void)bs_vm_register_builtin(vm, "ini_section_exists", bs_builtin_ini_section_exists);
  (void)bs_vm_register_builtin(vm, "ini_read_real", bs_builtin_ini_read_real);
  (void)bs_vm_register_builtin(vm, "ini_read_string", bs_builtin_ini_read_string);
  (void)bs_vm_register_builtin(vm, "ini_write_real", bs_builtin_ini_write_real);
  (void)bs_vm_register_builtin(vm, "file_exists", bs_builtin_file_exists);
  (void)bs_vm_register_builtin(vm, "action_move_to", bs_builtin_action_move_to);
  (void)bs_vm_register_builtin(vm, "action_set_alarm", bs_builtin_action_set_alarm);
  (void)bs_vm_register_builtin(vm, "action_kill_object", bs_builtin_action_kill_object);
  (void)bs_vm_register_builtin(vm, "event_inherited", bs_builtin_event_inherited);
  (void)bs_vm_register_builtin(vm, "event_user", bs_builtin_event_user);
  (void)bs_vm_register_builtin(vm, "event_perform", bs_builtin_event_perform);
  (void)bs_vm_register_builtin(vm, "keyboard_check", bs_builtin_keyboard_check);
  (void)bs_vm_register_builtin(vm, "keyboard_check_pressed", bs_builtin_keyboard_check_pressed);
  (void)bs_vm_register_builtin(vm, "keyboard_check_released", bs_builtin_keyboard_check_released);
  (void)bs_vm_register_builtin(vm, "keyboard_check_direct", bs_builtin_keyboard_check_direct);
  (void)bs_vm_register_builtin(vm, "keyboard_key_press", bs_builtin_keyboard_key_press);
  (void)bs_vm_register_builtin(vm, "keyboard_key_release", bs_builtin_keyboard_key_release);
  (void)bs_vm_register_builtin(vm, "keyboard_clear", bs_builtin_keyboard_clear);
  (void)bs_vm_register_builtin(vm, "collision_point", bs_builtin_collision_point);
  (void)bs_vm_register_builtin(vm, "collision_rectangle", bs_builtin_collision_rectangle);
  (void)bs_vm_register_builtin(vm, "collision_circle", bs_builtin_collision_circle);
  (void)bs_vm_register_builtin(vm, "collision_line", bs_builtin_collision_line);
  (void)bs_vm_register_builtin(vm, "room_exists", bs_builtin_room_exists);
  (void)bs_vm_register_builtin(vm, "room_next", bs_builtin_room_next);
  (void)bs_vm_register_builtin(vm, "room_previous", bs_builtin_room_previous);
  (void)bs_vm_register_builtin(vm, "random", bs_builtin_random);
  (void)bs_vm_register_builtin(vm, "irandom", bs_builtin_irandom);
  (void)bs_vm_register_builtin(vm, "min", bs_builtin_min);
  (void)bs_vm_register_builtin(vm, "max", bs_builtin_max);
  (void)bs_vm_register_builtin(vm, "audio_play_sound", bs_builtin_audio_play_sound);
  (void)bs_vm_register_builtin(vm, "audio_sound_pitch", bs_builtin_audio_sound_pitch);
  (void)bs_vm_register_builtin(vm, "audio_sound_gain", bs_builtin_audio_sound_gain);
  (void)bs_vm_register_builtin(vm, "audio_stop_sound", bs_builtin_audio_stop_sound);
  (void)bs_vm_register_builtin(vm, "audio_stop_all", bs_builtin_audio_stop_all);
  (void)bs_vm_register_builtin(vm, "audio_is_playing", bs_builtin_audio_is_playing);
  (void)bs_vm_register_builtin(vm, "audio_pause_sound", bs_builtin_audio_pause_sound);
  (void)bs_vm_register_builtin(vm, "audio_resume_sound", bs_builtin_audio_resume_sound);
  (void)bs_vm_register_builtin(vm, "audio_master_gain", bs_builtin_audio_master_gain);
  (void)bs_vm_register_builtin(vm, "audio_sound_set_track_position", bs_builtin_audio_sound_set_track_position);
  (void)bs_vm_register_builtin(vm, "audio_sound_get_track_position", bs_builtin_audio_sound_get_track_position);
  (void)bs_vm_register_builtin(vm, "draw_self", bs_builtin_draw_self);
  (void)bs_vm_register_builtin(vm, "draw_set_color", bs_builtin_draw_set_color);
  (void)bs_vm_register_builtin(vm, "draw_get_color", bs_builtin_draw_get_color);
  (void)bs_vm_register_builtin(vm, "draw_set_alpha", bs_builtin_draw_set_alpha);
  (void)bs_vm_register_builtin(vm, "draw_get_alpha", bs_builtin_draw_get_alpha);
  (void)bs_vm_register_builtin(vm, "draw_set_halign", bs_builtin_draw_set_halign);
  (void)bs_vm_register_builtin(vm, "draw_set_valign", bs_builtin_draw_set_valign);
  (void)bs_vm_register_builtin(vm, "draw_rectangle", bs_builtin_draw_rectangle);
  (void)bs_vm_register_builtin(vm, "ossafe_fill_rectangle", bs_builtin_ossafe_fill_rectangle);
  (void)bs_vm_register_builtin(vm, "draw_set_font", bs_builtin_draw_set_font);
  (void)bs_vm_register_builtin(vm, "draw_sprite", bs_builtin_draw_sprite);
  (void)bs_vm_register_builtin(vm, "draw_sprite_ext", bs_builtin_draw_sprite_ext);
  (void)bs_vm_register_builtin(vm, "draw_sprite_part", bs_builtin_draw_sprite_part);
  (void)bs_vm_register_builtin(vm, "draw_sprite_part_ext", bs_builtin_draw_sprite_part_ext);
  (void)bs_vm_register_builtin(vm, "draw_text", bs_builtin_draw_text);
  (void)bs_vm_register_builtin(vm, "draw_text_ext", bs_builtin_draw_text_ext);
  (void)bs_vm_register_builtin(vm, "draw_text_transformed", bs_builtin_draw_text_transformed);
  (void)bs_vm_register_builtin(vm, "draw_background", bs_builtin_draw_background);
  (void)bs_vm_register_builtin(vm, "draw_background_ext", bs_builtin_draw_background_ext);
  (void)bs_vm_register_builtin(vm, "surface_get_width", bs_builtin_surface_get_width);
  (void)bs_vm_register_builtin(vm, "surface_get_height", bs_builtin_surface_get_height);

  printf("Builtins registered: runtime core + bootstrap stubs\n");
}
