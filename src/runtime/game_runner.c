#include "bs/runtime/game_runner.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void bs_game_runner_fire_event(bs_game_runner *runner,
                                      bs_instance *instance,
                                      int32_t event_type,
                                      int32_t subtype,
                                      bs_instance *other_instance);

static double bs_now_millis(void) {
  struct timespec ts;
  if (timespec_get(&ts, TIME_UTC) == TIME_UTC) {
    return ((double)ts.tv_sec * 1000.0) + ((double)ts.tv_nsec / 1000000.0);
  }
  return 0.0;
}

static const char *bs_vm_exit_reason_to_string(bs_vm_exit_reason reason) {
  switch (reason) {
    case BS_VM_EXIT_NONE:
      return "none";
    case BS_VM_EXIT_RET:
      return "ret";
    case BS_VM_EXIT_EXIT:
      return "exit";
    case BS_VM_EXIT_OUT_OF_RANGE:
      return "end";
    case BS_VM_EXIT_MAX_INSTRUCTIONS:
      return "max_instructions";
    case BS_VM_EXIT_ERROR:
      return "error";
    default:
      return "unknown";
  }
}

static bool bs_trace_intro_state_enabled(void) {
  static int initialized = 0;
  static bool enabled = false;
  if (!initialized) {
    const char *env = getenv("BS_TRACE_INTRO_STATE");
    enabled = (env != NULL && (strcmp(env, "1") == 0 || strcmp(env, "true") == 0));
    initialized = 1;
  }
  return enabled;
}

static bool bs_trace_vm_enabled(void) {
  static int initialized = 0;
  static bool enabled = false;
  if (!initialized) {
    const char *env = getenv("BS_TRACE_VM");
    enabled = (env != NULL && (strcmp(env, "1") == 0 || strcmp(env, "true") == 0));
    initialized = 1;
  }
  return enabled;
}

static bool bs_trace_frame_enabled(void) {
  static int initialized = 0;
  static bool enabled = false;
  if (!initialized) {
    const char *env = getenv("BS_TRACE_FRAME");
    enabled = (env != NULL && (strcmp(env, "1") == 0 || strcmp(env, "true") == 0));
    initialized = 1;
  }
  return enabled;
}

static void bs_game_runner_trace_intro_state(const bs_game_runner *runner) {
  if (runner == NULL || !bs_trace_intro_state_enabled()) {
    return;
  }
  for (size_t i = 0; i < runner->instance_count; i++) {
    const bs_instance *inst = &runner->instances[i];
    if (inst->destroyed) {
      continue;
    }
    if (inst->object_index == 100 || inst->object_index == 99 || inst->object_index == 784) {
      printf("  [INTRO] f=%llu obj=%d id=%d spr=%d img=%.3f spd=%.3f a0=%d a1=%d a2=%d vis=%d x=%.1f y=%.1f\n",
             (unsigned long long)runner->frame_count,
             inst->object_index,
             inst->id,
             inst->sprite_index,
             inst->image_index,
             inst->image_speed,
             inst->alarm[0],
             inst->alarm[1],
             inst->alarm[2],
             inst->visible ? 1 : 0,
             inst->x,
             inst->y);
    }
  }
}

static void bs_instance_dispose(bs_instance *instance) {
  if (instance == NULL) {
    return;
  }

  free(instance->variable_indices);
  free(instance->variable_values);
  instance->variable_indices = NULL;
  instance->variable_values = NULL;
  instance->variable_count = 0;
  instance->variable_capacity = 0;
}

static bool bs_instance_clone(const bs_instance *src, bs_instance *out_clone) {
  if (src == NULL || out_clone == NULL) {
    return false;
  }

  memcpy(out_clone, src, sizeof(*out_clone));
  out_clone->variable_indices = NULL;
  out_clone->variable_values = NULL;
  out_clone->variable_capacity = 0;

  if (src->variable_count == 0) {
    out_clone->variable_count = 0;
    return true;
  }

  out_clone->variable_indices = (int32_t *)malloc(src->variable_count * sizeof(int32_t));
  out_clone->variable_values = (double *)malloc(src->variable_count * sizeof(double));
  if (out_clone->variable_indices == NULL || out_clone->variable_values == NULL) {
    free(out_clone->variable_indices);
    free(out_clone->variable_values);
    out_clone->variable_indices = NULL;
    out_clone->variable_values = NULL;
    out_clone->variable_count = 0;
    out_clone->variable_capacity = 0;
    return false;
  }

  memcpy(out_clone->variable_indices, src->variable_indices, src->variable_count * sizeof(int32_t));
  memcpy(out_clone->variable_values, src->variable_values, src->variable_count * sizeof(double));
  out_clone->variable_count = src->variable_count;
  out_clone->variable_capacity = src->variable_count;
  return true;
}

static void bs_saved_room_state_clear(bs_saved_room_state *state) {
  if (state == NULL) {
    return;
  }
  if (state->instances != NULL) {
    for (size_t i = 0; i < state->instance_count; i++) {
      bs_instance_dispose(&state->instances[i]);
    }
  }
  free(state->instances);
  state->instances = NULL;
  state->instance_count = 0;
}

static void bs_game_runner_clear_all_saved_room_states(bs_game_runner *runner) {
  if (runner == NULL || runner->saved_room_states == NULL) {
    return;
  }
  for (size_t i = 0; i < runner->saved_room_state_count; i++) {
    bs_saved_room_state_clear(&runner->saved_room_states[i]);
  }
}

static void bs_game_runner_set_current_room_persistent(bs_game_runner *runner, bool persistent) {
  if (runner == NULL || runner->current_room_index < 0) {
    return;
  }
  if ((size_t)runner->current_room_index < runner->room_persistent_flag_count &&
      runner->room_persistent_flags != NULL) {
    runner->room_persistent_flags[(size_t)runner->current_room_index] = persistent;
  }
}

static bool bs_instance_set_dynamic_variable(bs_instance *instance, int32_t variable_index, double value) {
  if (instance == NULL || variable_index < 0) {
    return false;
  }

  for (size_t i = 0; i < instance->variable_count; i++) {
    if (instance->variable_indices[i] == variable_index) {
      instance->variable_values[i] = value;
      return true;
    }
  }

  if (instance->variable_count == instance->variable_capacity) {
    size_t new_capacity = (instance->variable_capacity == 0) ? 64u : (instance->variable_capacity * 2u);
    int32_t *grown_indices = (int32_t *)malloc(new_capacity * sizeof(int32_t));
    double *grown_values = (double *)malloc(new_capacity * sizeof(double));
    if (grown_indices == NULL || grown_values == NULL) {
      free(grown_indices);
      free(grown_values);
      return false;
    }

    if (instance->variable_count > 0) {
      memcpy(grown_indices, instance->variable_indices, instance->variable_count * sizeof(int32_t));
      memcpy(grown_values, instance->variable_values, instance->variable_count * sizeof(double));
    }

    free(instance->variable_indices);
    free(instance->variable_values);
    instance->variable_indices = grown_indices;
    instance->variable_values = grown_values;
    instance->variable_capacity = new_capacity;
  }

  instance->variable_indices[instance->variable_count] = variable_index;
  instance->variable_values[instance->variable_count] = value;
  instance->variable_count++;
  return true;
}

static bool bs_instance_try_get_dynamic_variable(const bs_instance *instance,
                                                 int32_t variable_index,
                                                 double *out_value) {
  if (instance == NULL || variable_index < 0) {
    return false;
  }

  for (size_t i = 0; i < instance->variable_count; i++) {
    if (instance->variable_indices[i] == variable_index) {
      if (out_value != NULL) {
        *out_value = instance->variable_values[i];
      }
      return true;
    }
  }

  return false;
}

static bool bs_game_runner_try_get_global_builtin(const bs_game_runner *runner,
                                                  const char *name,
                                                  double *out_value) {
  if (runner == NULL || name == NULL || out_value == NULL) {
    return false;
  }

  if (strcmp(name, "room") == 0) {
    *out_value = (double)runner->current_room_index;
    return true;
  }
  if (strcmp(name, "room_speed") == 0) {
    *out_value = (double)(runner->current_room != NULL ? runner->current_room->speed : 30);
    return true;
  }
  if (strcmp(name, "room_width") == 0) {
    *out_value = (double)(runner->current_room != NULL ? runner->current_room->width : 640);
    return true;
  }
  if (strcmp(name, "room_height") == 0) {
    *out_value = (double)(runner->current_room != NULL ? runner->current_room->height : 480);
    return true;
  }
  if (strcmp(name, "view_current") == 0) {
    *out_value = 0.0;
    return true;
  }
  if (strcmp(name, "current_time") == 0) {
    *out_value = bs_now_millis();
    return true;
  }
  if (strcmp(name, "fps") == 0) {
    *out_value = (double)(runner->current_room != NULL ? runner->current_room->speed : 30);
    return true;
  }
  if (strcmp(name, "instance_count") == 0) {
    *out_value = (double)runner->instance_count;
    return true;
  }
  if (strcmp(name, "keyboard_key") == 0) {
    *out_value = (double)runner->keyboard_key;
    return true;
  }
  if (strcmp(name, "keyboard_lastkey") == 0) {
    *out_value = (double)runner->keyboard_lastkey;
    return true;
  }
  if (strcmp(name, "mouse_x") == 0 || strcmp(name, "mouse_y") == 0) {
    *out_value = 0.0;
    return true;
  }
  if (strcmp(name, "os_type") == 0) {
    *out_value = 1.0;
    return true;
  }
  if (strcmp(name, "game_id") == 0) {
    *out_value = (double)(runner->game_data != NULL ? runner->game_data->gen8.game_id : 0u);
    return true;
  }
  if (strcmp(name, "browser_width") == 0) {
    *out_value = (double)(runner->game_data != NULL ? runner->game_data->gen8.window_width : 640u);
    return true;
  }
  if (strcmp(name, "browser_height") == 0) {
    *out_value = (double)(runner->game_data != NULL ? runner->game_data->gen8.window_height : 480u);
    return true;
  }
  if (strcmp(name, "room_persistent") == 0) {
    if (runner->current_room_index >= 0 &&
        (size_t)runner->current_room_index < runner->room_persistent_flag_count &&
        runner->room_persistent_flags != NULL) {
      *out_value = runner->room_persistent_flags[(size_t)runner->current_room_index] ? 1.0 : 0.0;
    } else {
      *out_value = (runner->current_room != NULL && runner->current_room->persistent) ? 1.0 : 0.0;
    }
    return true;
  }
  if (strcmp(name, "display_aa") == 0) {
    *out_value = 0.0;
    return true;
  }
  if (strcmp(name, "application_surface") == 0) {
    *out_value = -1.0;
    return true;
  }
  if (strcmp(name, "path_action_stop") == 0) {
    *out_value = 0.0;
    return true;
  }
  if (strcmp(name, "path_action_restart") == 0) {
    *out_value = 1.0;
    return true;
  }
  if (strcmp(name, "path_action_continue") == 0) {
    *out_value = 2.0;
    return true;
  }
  if (strcmp(name, "path_action_reverse") == 0) {
    *out_value = 3.0;
    return true;
  }

  return false;
}

static void bs_game_runner_clear_instances(bs_game_runner *runner) {
  if (runner == NULL) {
    return;
  }
  if (runner->instances != NULL) {
    for (size_t i = 0; i < runner->instance_count; i++) {
      bs_instance_dispose(&runner->instances[i]);
    }
  }
  free(runner->instances);
  runner->instances = NULL;
  runner->instance_count = 0;
  runner->instance_capacity = 0;
}

static bool bs_game_runner_ensure_instance_capacity(bs_game_runner *runner, size_t needed) {
  if (runner == NULL) {
    return false;
  }

  if (needed <= runner->instance_capacity) {
    return true;
  }

  {
    size_t new_capacity = (runner->instance_capacity == 0) ? 64u : (runner->instance_capacity * 2u);
    while (new_capacity < needed) {
      new_capacity *= 2u;
    }
    bs_instance *grown = (bs_instance *)realloc(runner->instances, new_capacity * sizeof(bs_instance));
    if (grown == NULL) {
      return false;
    }
    runner->instances = grown;
    runner->instance_capacity = new_capacity;
  }
  return true;
}

static bs_instance *bs_game_runner_create_instance(bs_game_runner *runner,
                                                   int32_t object_index,
                                                   double x,
                                                   double y,
                                                   int32_t preferred_id) {
  bs_instance *instance = NULL;
  if (runner == NULL) {
    return NULL;
  }
  if (!bs_game_runner_ensure_instance_capacity(runner, runner->instance_count + 1u)) {
    return NULL;
  }

  instance = &runner->instances[runner->instance_count];
  memset(instance, 0, sizeof(*instance));
  instance->id = (preferred_id >= 0) ? preferred_id : runner->next_instance_id;
  instance->object_index = object_index;
  instance->x = x;
  instance->y = y;
  instance->xprevious = x;
  instance->yprevious = y;
  instance->xstart = x;
  instance->ystart = y;
  instance->hspeed = 0.0;
  instance->vspeed = 0.0;
  instance->speed = 0.0;
  instance->direction = 0.0;
  instance->friction = 0.0;
  instance->gravity = 0.0;
  instance->gravity_direction = 270.0;
  if (object_index >= 0 && (size_t)object_index < runner->game_data->object_count) {
    const bs_game_object_data *obj = &runner->game_data->objects[(size_t)object_index];
    instance->mask_index = obj->mask_id;
    instance->sprite_index = obj->sprite_index;
    instance->depth = obj->depth;
    instance->visible = obj->visible;
    instance->solid = obj->solid;
    instance->persistent = obj->persistent;
  } else {
    instance->mask_index = -1;
    instance->sprite_index = -1;
    instance->depth = 0;
    instance->visible = true;
    instance->solid = false;
    instance->persistent = false;
  }
  instance->image_index = 0.0;
  instance->image_speed = 1.0;
  instance->image_xscale = 1.0;
  instance->image_yscale = 1.0;
  instance->image_angle = 0.0;
  instance->image_alpha = 1.0;
  instance->image_single = -1.0;
  instance->image_blend = 0x00FFFFFF;
  instance->path_index = -1;
  instance->path_position = 0.0;
  instance->path_speed = 0.0;
  instance->path_end_action = 0;
  instance->path_orientation = 0.0;
  instance->path_scale = 1.0;
  instance->path_x_offset = 0.0;
  instance->path_y_offset = 0.0;
  instance->variable_indices = NULL;
  instance->variable_values = NULL;
  instance->variable_count = 0;
  instance->variable_capacity = 0;
  for (size_t i = 0; i < 12; i++) {
    instance->alarm[i] = -1;
  }
  instance->has_been_marked_as_outside_room = false;
  instance->destroyed = false;
  runner->instance_count++;

  if (instance->id >= runner->next_instance_id) {
    runner->next_instance_id = instance->id + 1;
  }

  return instance;
}

bs_instance *bs_game_runner_find_instance_by_id(bs_game_runner *runner, int32_t id) {
  if (runner == NULL) {
    return NULL;
  }

  for (size_t i = 0; i < runner->instance_count; i++) {
    if (runner->instances[i].id == id) {
      return &runner->instances[i];
    }
  }

  return NULL;
}

bool bs_game_runner_object_is_child_of(const bs_game_runner *runner,
                                       int32_t child_object_index,
                                       int32_t parent_object_index) {
  int32_t current = child_object_index;
  int depth = 0;
  if (runner == NULL || runner->game_data == NULL) {
    return false;
  }
  if (child_object_index < 0 || parent_object_index < 0) {
    return false;
  }
  if (child_object_index == parent_object_index) {
    return true;
  }

  while (depth < 64) {
    const bs_game_object_data *obj = NULL;
    if (current < 0 || (size_t)current >= runner->game_data->object_count) {
      return false;
    }
    obj = &runner->game_data->objects[(size_t)current];
    if (obj->parent_id == parent_object_index) {
      return true;
    }
    if (obj->parent_id < 0) {
      return false;
    }
    current = obj->parent_id;
    depth++;
  }

  return false;
}

static size_t bs_game_runner_sprite_frame_count(const bs_game_runner *runner, int32_t sprite_index) {
  if (runner == NULL || runner->game_data == NULL) {
    return 0;
  }
  if (sprite_index < 0 || (size_t)sprite_index >= runner->game_data->sprite_count) {
    return 0;
  }
  if (runner->game_data->sprites[(size_t)sprite_index].subimage_count == 0) {
    return 1;
  }
  return runner->game_data->sprites[(size_t)sprite_index].subimage_count;
}

double bs_game_runner_instance_get_variable(bs_game_runner *runner,
                                            int32_t instance_id,
                                            int32_t variable_index,
                                            const char *variable_name) {
  bs_instance *instance = bs_game_runner_find_instance_by_id(runner, instance_id);
  double value = 0.0;
  if (instance == NULL) {
    if (bs_game_runner_try_get_global_builtin(runner, variable_name, &value)) {
      return value;
    }
    return 0.0;
  }

  if (variable_name != NULL) {
    if (strcmp(variable_name, "x") == 0) {
      return instance->x;
    }
    if (strcmp(variable_name, "y") == 0) {
      return instance->y;
    }
    if (strcmp(variable_name, "xprevious") == 0) {
      return instance->xprevious;
    }
    if (strcmp(variable_name, "yprevious") == 0) {
      return instance->yprevious;
    }
    if (strcmp(variable_name, "xstart") == 0) {
      return instance->xstart;
    }
    if (strcmp(variable_name, "ystart") == 0) {
      return instance->ystart;
    }
    if (strcmp(variable_name, "hspeed") == 0) {
      return instance->hspeed;
    }
    if (strcmp(variable_name, "vspeed") == 0) {
      return instance->vspeed;
    }
    if (strcmp(variable_name, "speed") == 0) {
      return instance->speed;
    }
    if (strcmp(variable_name, "direction") == 0) {
      return instance->direction;
    }
    if (strcmp(variable_name, "friction") == 0) {
      return instance->friction;
    }
    if (strcmp(variable_name, "gravity") == 0) {
      return instance->gravity;
    }
    if (strcmp(variable_name, "gravity_direction") == 0) {
      return instance->gravity_direction;
    }
    if (strcmp(variable_name, "id") == 0) {
      return (double)instance->id;
    }
    if (strcmp(variable_name, "object_index") == 0) {
      return (double)instance->object_index;
    }
    if (strcmp(variable_name, "sprite_index") == 0) {
      return (double)instance->sprite_index;
    }
    if (strcmp(variable_name, "mask_index") == 0) {
      return (double)instance->mask_index;
    }
    if (strcmp(variable_name, "depth") == 0) {
      return (double)instance->depth;
    }
    if (strcmp(variable_name, "visible") == 0) {
      return instance->visible ? 1.0 : 0.0;
    }
    if (strcmp(variable_name, "solid") == 0) {
      return instance->solid ? 1.0 : 0.0;
    }
    if (strcmp(variable_name, "persistent") == 0) {
      return instance->persistent ? 1.0 : 0.0;
    }
    if (strcmp(variable_name, "image_index") == 0) {
      return instance->image_index;
    }
    if (strcmp(variable_name, "image_speed") == 0) {
      return instance->image_speed;
    }
    if (strcmp(variable_name, "image_xscale") == 0) {
      return instance->image_xscale;
    }
    if (strcmp(variable_name, "image_yscale") == 0) {
      return instance->image_yscale;
    }
    if (strcmp(variable_name, "image_angle") == 0) {
      return instance->image_angle;
    }
    if (strcmp(variable_name, "image_alpha") == 0) {
      return instance->image_alpha;
    }
    if (strcmp(variable_name, "image_single") == 0) {
      return instance->image_single;
    }
    if (strcmp(variable_name, "image_blend") == 0) {
      return (double)instance->image_blend;
    }
    if (strcmp(variable_name, "image_number") == 0) {
      return (double)bs_game_runner_sprite_frame_count(runner, instance->sprite_index);
    }
    if (strcmp(variable_name, "path_index") == 0) {
      return (double)instance->path_index;
    }
    if (strcmp(variable_name, "path_position") == 0) {
      return instance->path_position;
    }
    if (strcmp(variable_name, "path_speed") == 0) {
      return instance->path_speed;
    }
    if (strcmp(variable_name, "path_endaction") == 0) {
      return (double)instance->path_end_action;
    }
    if (strcmp(variable_name, "path_orientation") == 0) {
      return instance->path_orientation;
    }
    if (strcmp(variable_name, "path_scale") == 0) {
      return instance->path_scale;
    }
    if (strcmp(variable_name, "room_persistent") == 0) {
      double persistent = 0.0;
      if (runner != NULL &&
          runner->current_room_index >= 0 &&
          (size_t)runner->current_room_index < runner->room_persistent_flag_count &&
          runner->room_persistent_flags != NULL &&
          runner->room_persistent_flags[(size_t)runner->current_room_index]) {
        persistent = 1.0;
      }
      return persistent;
    }
    if (strcmp(variable_name, "bbox_left") == 0) {
      bs_bbox bbox = {0};
      if (bs_game_runner_compute_instance_bbox(runner, instance, &bbox)) {
        return floor(bbox.left);
      }
      return 0.0;
    }
    if (strcmp(variable_name, "bbox_right") == 0) {
      bs_bbox bbox = {0};
      if (bs_game_runner_compute_instance_bbox(runner, instance, &bbox)) {
        return ceil(bbox.right - 1.0);
      }
      return 0.0;
    }
    if (strcmp(variable_name, "bbox_top") == 0) {
      bs_bbox bbox = {0};
      if (bs_game_runner_compute_instance_bbox(runner, instance, &bbox)) {
        return floor(bbox.top);
      }
      return 0.0;
    }
    if (strcmp(variable_name, "bbox_bottom") == 0) {
      bs_bbox bbox = {0};
      if (bs_game_runner_compute_instance_bbox(runner, instance, &bbox)) {
        return ceil(bbox.bottom - 1.0);
      }
      return 0.0;
    }
    if (strcmp(variable_name, "sprite_width") == 0) {
      if (runner != NULL &&
          runner->game_data != NULL &&
          instance->sprite_index >= 0 &&
          (size_t)instance->sprite_index < runner->game_data->sprite_count) {
        const bs_sprite_data *sprite = &runner->game_data->sprites[(size_t)instance->sprite_index];
        return (double)sprite->width * fabs(instance->image_xscale);
      }
      return 0.0;
    }
    if (strcmp(variable_name, "sprite_height") == 0) {
      if (runner != NULL &&
          runner->game_data != NULL &&
          instance->sprite_index >= 0 &&
          (size_t)instance->sprite_index < runner->game_data->sprite_count) {
        const bs_sprite_data *sprite = &runner->game_data->sprites[(size_t)instance->sprite_index];
        return (double)sprite->height * fabs(instance->image_yscale);
      }
      return 0.0;
    }
  }

  if (bs_instance_try_get_dynamic_variable(instance, variable_index, &value)) {
    return value;
  }
  if (bs_game_runner_try_get_global_builtin(runner, variable_name, &value)) {
    return value;
  }
  return 0.0;
}

bool bs_game_runner_instance_set_variable(bs_game_runner *runner,
                                          int32_t instance_id,
                                          int32_t variable_index,
                                          const char *variable_name,
                                          double value) {
  bs_instance *instance = bs_game_runner_find_instance_by_id(runner, instance_id);
  const double pi = 3.14159265358979323846;
  if (instance == NULL) {
    return false;
  }

  if (variable_name != NULL) {
    if (strcmp(variable_name, "x") == 0) {
      instance->x = value;
      return true;
    }
    if (strcmp(variable_name, "y") == 0) {
      instance->y = value;
      return true;
    }
    if (strcmp(variable_name, "xprevious") == 0) {
      instance->xprevious = value;
      return true;
    }
    if (strcmp(variable_name, "yprevious") == 0) {
      instance->yprevious = value;
      return true;
    }
    if (strcmp(variable_name, "xstart") == 0) {
      instance->xstart = value;
      return true;
    }
    if (strcmp(variable_name, "ystart") == 0) {
      instance->ystart = value;
      return true;
    }
    if (strcmp(variable_name, "hspeed") == 0) {
      instance->hspeed = value;
      instance->speed = sqrt(instance->hspeed * instance->hspeed + instance->vspeed * instance->vspeed);
      instance->direction = fmod((atan2(-instance->vspeed, instance->hspeed) * (180.0 / pi)) + 360.0, 360.0);
      return true;
    }
    if (strcmp(variable_name, "vspeed") == 0) {
      instance->vspeed = value;
      instance->speed = sqrt(instance->hspeed * instance->hspeed + instance->vspeed * instance->vspeed);
      instance->direction = fmod((atan2(-instance->vspeed, instance->hspeed) * (180.0 / pi)) + 360.0, 360.0);
      return true;
    }
    if (strcmp(variable_name, "speed") == 0) {
      instance->speed = value;
      instance->hspeed = instance->speed * cos(instance->direction * (pi / 180.0));
      instance->vspeed = -instance->speed * sin(instance->direction * (pi / 180.0));
      return true;
    }
    if (strcmp(variable_name, "direction") == 0) {
      instance->direction = value;
      instance->hspeed = instance->speed * cos(instance->direction * (pi / 180.0));
      instance->vspeed = -instance->speed * sin(instance->direction * (pi / 180.0));
      return true;
    }
    if (strcmp(variable_name, "friction") == 0) {
      instance->friction = value;
      return true;
    }
    if (strcmp(variable_name, "gravity") == 0) {
      instance->gravity = value;
      return true;
    }
    if (strcmp(variable_name, "gravity_direction") == 0) {
      instance->gravity_direction = value;
      return true;
    }
    if (strcmp(variable_name, "sprite_index") == 0) {
      instance->sprite_index = (int32_t)value;
      instance->image_index = 0.0;
      return true;
    }
    if (strcmp(variable_name, "mask_index") == 0) {
      instance->mask_index = (int32_t)value;
      return true;
    }
    if (strcmp(variable_name, "depth") == 0) {
      instance->depth = (int32_t)value;
      return true;
    }
    if (strcmp(variable_name, "visible") == 0) {
      instance->visible = (value != 0.0);
      return true;
    }
    if (strcmp(variable_name, "solid") == 0) {
      instance->solid = (value != 0.0);
      return true;
    }
    if (strcmp(variable_name, "persistent") == 0) {
      instance->persistent = (value != 0.0);
      return true;
    }
    if (strcmp(variable_name, "image_index") == 0) {
      instance->image_index = value;
      return true;
    }
    if (strcmp(variable_name, "image_speed") == 0) {
      instance->image_speed = value;
      return true;
    }
    if (strcmp(variable_name, "image_xscale") == 0) {
      instance->image_xscale = value;
      return true;
    }
    if (strcmp(variable_name, "image_yscale") == 0) {
      instance->image_yscale = value;
      return true;
    }
    if (strcmp(variable_name, "image_angle") == 0) {
      instance->image_angle = value;
      return true;
    }
    if (strcmp(variable_name, "image_alpha") == 0) {
      if (value < 0.0) {
        value = 0.0;
      }
      if (value > 1.0) {
        value = 1.0;
      }
      instance->image_alpha = value;
      return true;
    }
    if (strcmp(variable_name, "image_single") == 0) {
      instance->image_single = value;
      return true;
    }
    if (strcmp(variable_name, "image_blend") == 0) {
      int64_t color = (int64_t)value;
      if (color < 0) {
        color = 0;
      }
      if (color > 0xFFFFFF) {
        color = 0xFFFFFF;
      }
      instance->image_blend = (int32_t)color;
      return true;
    }
    if (strcmp(variable_name, "path_index") == 0) {
      instance->path_index = (int32_t)value;
      return true;
    }
    if (strcmp(variable_name, "path_position") == 0) {
      instance->path_position = value;
      return true;
    }
    if (strcmp(variable_name, "path_speed") == 0) {
      instance->path_speed = value;
      return true;
    }
    if (strcmp(variable_name, "path_endaction") == 0) {
      instance->path_end_action = (int32_t)value;
      return true;
    }
    if (strcmp(variable_name, "path_orientation") == 0) {
      instance->path_orientation = value;
      return true;
    }
    if (strcmp(variable_name, "path_scale") == 0) {
      instance->path_scale = value;
      return true;
    }
    if (strcmp(variable_name, "room_persistent") == 0) {
      bs_game_runner_set_current_room_persistent(runner, value != 0.0);
      return true;
    }
  }

  return bs_instance_set_dynamic_variable(instance, variable_index, value);
}

void bs_game_runner_destroy_instance(bs_game_runner *runner, int32_t id) {
  bs_instance *instance = bs_game_runner_find_instance_by_id(runner, id);
  if (instance != NULL && !instance->destroyed) {
    bs_game_runner_fire_event(runner, instance, BS_EVENT_DESTROY, 0, NULL);
    instance->destroyed = true;
  }
}

static const bs_event_entry *bs_game_runner_find_event_in_object_chain(const bs_game_runner *runner,
                                                                       int32_t object_index,
                                                                       int32_t event_type,
                                                                       int32_t subtype,
                                                                       int32_t *out_owner_object_index) {
  int32_t current = object_index;
  int depth = 0;
  if (runner == NULL || runner->game_data == NULL || event_type < 0) {
    return NULL;
  }

  while (depth < 64) {
    const bs_game_object_data *object_data = NULL;
    if (current < 0 || (size_t)current >= runner->game_data->object_count) {
      return NULL;
    }

    object_data = &runner->game_data->objects[(size_t)current];
    if ((size_t)event_type < object_data->event_type_count) {
      const bs_object_event_list *event_list = &object_data->events[(size_t)event_type];
      for (size_t i = 0; i < event_list->entry_count; i++) {
        if (event_list->entries[i].subtype == subtype) {
          if (out_owner_object_index != NULL) {
            *out_owner_object_index = current;
          }
          return &event_list->entries[i];
        }
      }
    }

    current = object_data->parent_id;
    if (current < 0) {
      return NULL;
    }
    depth++;
  }

  return NULL;
}

static void bs_game_runner_execute_event_entry(bs_game_runner *runner,
                                               bs_instance *instance,
                                               int32_t event_type,
                                               int32_t subtype,
                                               int32_t owner_object_index,
                                               const bs_event_entry *event_entry,
                                               int32_t other_instance_id) {
  int32_t prev_self_id = 0;
  int32_t prev_other_id = 0;
  bool prev_context_active = false;
  int32_t prev_event_type = 0;
  int32_t prev_event_subtype = 0;
  int32_t prev_event_object_index = 0;
  const bool trace_vm = bs_trace_vm_enabled();
  if (runner == NULL ||
      instance == NULL ||
      instance->destroyed ||
      runner->vm == NULL ||
      event_entry == NULL) {
    return;
  }

  prev_self_id = runner->vm->current_self_id;
  prev_other_id = runner->vm->current_other_id;
  prev_context_active = runner->event_context_active;
  prev_event_type = runner->current_event_type;
  prev_event_subtype = runner->current_event_subtype;
  prev_event_object_index = runner->current_event_object_index;
  runner->vm->current_self_id = instance->id;
  runner->vm->current_other_id = other_instance_id;
  runner->event_context_active = true;
  runner->current_event_type = event_type;
  runner->current_event_subtype = subtype;
  runner->current_event_object_index = owner_object_index;

  for (size_t i = 0; i < event_entry->action_count; i++) {
    int32_t code_id = event_entry->actions[i].code_id;
    if (code_id >= 0 && (size_t)code_id < runner->game_data->code_entry_count) {
      bs_vm_execute_result result = {0};
      if (runner->trace_events) {
        const bs_code_entry_data *trace_entry = &runner->game_data->code_entries[(size_t)code_id];
        printf("  [EVT] room=%d obj=%d inst=%d evt=%d sub=%d code=%d name=%s\n",
               runner->current_room_index,
               instance->object_index,
               instance->id,
               event_type,
               subtype,
               code_id,
               trace_entry->name != NULL ? trace_entry->name : "<unnamed>");
      }
      bool ok = bs_vm_execute_code(runner->vm,
                                   (size_t)code_id,
                                   120000u,
                                   (runner->trace_events && trace_vm),
                                   &result);
      runner->total_vm_event_calls++;
      runner->total_vm_instructions += result.instructions_executed;
      if (!ok || result.exit_reason == BS_VM_EXIT_ERROR) {
        const bs_code_entry_data *entry = &runner->game_data->code_entries[(size_t)code_id];
        printf("  VM event error: obj=%d inst=%d code=%d name=%s ok=%s reason=%s instructions=%u\n",
               instance->object_index,
               instance->id,
               code_id,
               entry->name != NULL ? entry->name : "<unnamed>",
               ok ? "true" : "false",
               bs_vm_exit_reason_to_string(result.exit_reason),
               (unsigned)result.instructions_executed);
      }
    }
  }

  runner->vm->current_self_id = prev_self_id;
  runner->vm->current_other_id = prev_other_id;
  runner->event_context_active = prev_context_active;
  runner->current_event_type = prev_event_type;
  runner->current_event_subtype = prev_event_subtype;
  runner->current_event_object_index = prev_event_object_index;
}

void bs_game_runner_fire_event_inherited(bs_game_runner *runner, bs_instance *instance) {
  const bs_event_entry *event_entry = NULL;
  int32_t owner_object_index = -1;
  int32_t parent_object_index = -1;
  if (runner == NULL ||
      instance == NULL ||
      instance->destroyed ||
      runner->game_data == NULL ||
      !runner->event_context_active ||
      runner->current_event_object_index < 0 ||
      (size_t)runner->current_event_object_index >= runner->game_data->object_count) {
    return;
  }

  parent_object_index = runner->game_data->objects[(size_t)runner->current_event_object_index].parent_id;
  if (parent_object_index < 0) {
    return;
  }

  event_entry = bs_game_runner_find_event_in_object_chain(runner,
                                                           parent_object_index,
                                                           runner->current_event_type,
                                                           runner->current_event_subtype,
                                                           &owner_object_index);
  if (event_entry == NULL) {
    return;
  }

  bs_game_runner_execute_event_entry(runner,
                                     instance,
                                     runner->current_event_type,
                                     runner->current_event_subtype,
                                     owner_object_index,
                                     event_entry,
                                     instance->id);
}

static void bs_game_runner_fire_event(bs_game_runner *runner,
                                      bs_instance *instance,
                                      int32_t event_type,
                                      int32_t subtype,
                                      bs_instance *other_instance) {
  const bs_event_entry *event_entry = NULL;
  int32_t owner_object_index = -1;
  if (runner == NULL || instance == NULL || instance->destroyed || runner->vm == NULL) {
    return;
  }

  event_entry = bs_game_runner_find_event_in_object_chain(runner,
                                                           instance->object_index,
                                                           event_type,
                                                           subtype,
                                                           &owner_object_index);
  if (event_entry == NULL) {
    return;
  }

  bs_game_runner_execute_event_entry(runner,
                                     instance,
                                     event_type,
                                     subtype,
                                     owner_object_index,
                                     event_entry,
                                     other_instance != NULL ? other_instance->id : instance->id);
}

void bs_game_runner_fire_event_for_instance(bs_game_runner *runner,
                                            bs_instance *instance,
                                            int32_t event_type,
                                            int32_t subtype) {
  bs_game_runner_fire_event(runner, instance, event_type, subtype, NULL);
}

void bs_game_runner_path_end_instance(bs_game_runner *runner, bs_instance *instance) {
  (void)runner;
  if (instance == NULL) {
    return;
  }
  instance->path_index = -1;
  instance->path_speed = 0.0;
}

bs_instance *bs_game_runner_create_instance_runtime(bs_game_runner *runner,
                                                    int32_t object_index,
                                                    double x,
                                                    double y,
                                                    bool run_create_event) {
  bs_instance *created = NULL;
  if (runner == NULL || runner->game_data == NULL || object_index < 0) {
    return NULL;
  }
  if ((size_t)object_index >= runner->game_data->object_count) {
    return NULL;
  }

  created = bs_game_runner_create_instance(runner, object_index, x, y, -1);
  if (created == NULL) {
    return NULL;
  }

  if (run_create_event) {
    bs_game_runner_fire_event(runner, created, BS_EVENT_CREATE, 0, NULL);
  }
  return created;
}

static void bs_game_runner_dispatch_event_all(bs_game_runner *runner, int32_t event_type, int32_t subtype) {
  size_t snapshot_count = 0;
  if (runner == NULL) {
    return;
  }

  snapshot_count = runner->instance_count;
  for (size_t i = 0; i < snapshot_count; i++) {
    if (i >= runner->instance_count) {
      break;
    }
    bs_game_runner_fire_event(runner, &runner->instances[i], event_type, subtype, NULL);
  }
}

static bool bs_game_runner_contains_subtype(const int32_t *subtypes, size_t count, int32_t subtype) {
  for (size_t i = 0; i < count; i++) {
    if (subtypes[i] == subtype) {
      return true;
    }
  }
  return false;
}

bool bs_game_runner_compute_instance_bbox(const bs_game_runner *runner,
                                          const bs_instance *instance,
                                          bs_bbox *out_bbox) {
  int32_t sprite_index = 0;
  const bs_sprite_data *sprite = NULL;
  double x1 = 0.0;
  double x2 = 0.0;
  double y1 = 0.0;
  double y2 = 0.0;
  if (runner == NULL ||
      instance == NULL ||
      out_bbox == NULL ||
      runner->game_data == NULL) {
    return false;
  }

  sprite_index = (instance->mask_index >= 0) ? instance->mask_index : instance->sprite_index;
  if (sprite_index < 0 || (size_t)sprite_index >= runner->game_data->sprite_count) {
    return false;
  }
  sprite = &runner->game_data->sprites[(size_t)sprite_index];

  x1 = instance->x + (((double)sprite->margin_left - (double)sprite->origin_x) * instance->image_xscale);
  x2 = instance->x + ((((double)sprite->margin_right + 1.0) - (double)sprite->origin_x) * instance->image_xscale);
  y1 = instance->y + (((double)sprite->margin_top - (double)sprite->origin_y) * instance->image_yscale);
  y2 = instance->y + ((((double)sprite->margin_bottom + 1.0) - (double)sprite->origin_y) * instance->image_yscale);

  out_bbox->left = fmin(x1, x2);
  out_bbox->right = fmax(x1, x2);
  out_bbox->top = fmin(y1, y2);
  out_bbox->bottom = fmax(y1, y2);
  return true;
}

bool bs_game_runner_instances_overlap(const bs_game_runner *runner,
                                      const bs_instance *a,
                                      const bs_instance *b) {
  bs_bbox a_bbox = {0};
  bs_bbox b_bbox = {0};
  if (!bs_game_runner_compute_instance_bbox(runner, a, &a_bbox) ||
      !bs_game_runner_compute_instance_bbox(runner, b, &b_bbox)) {
    return false;
  }
  return a_bbox.left < b_bbox.right &&
         a_bbox.right > b_bbox.left &&
         a_bbox.top < b_bbox.bottom &&
         a_bbox.bottom > b_bbox.top;
}

static size_t bs_game_runner_collect_collision_targets(const bs_game_runner *runner,
                                                       int32_t object_index,
                                                       int32_t *out_targets,
                                                       size_t max_targets) {
  int32_t current = object_index;
  int depth = 0;
  size_t count = 0;
  if (runner == NULL || runner->game_data == NULL || out_targets == NULL || max_targets == 0) {
    return 0;
  }

  while (depth < 64) {
    const bs_game_object_data *obj = NULL;
    const bs_object_event_list *collision_events = NULL;
    if (current < 0 || (size_t)current >= runner->game_data->object_count) {
      break;
    }
    obj = &runner->game_data->objects[(size_t)current];
    if ((size_t)BS_EVENT_COLLISION < obj->event_type_count) {
      collision_events = &obj->events[(size_t)BS_EVENT_COLLISION];
      for (size_t i = 0; i < collision_events->entry_count; i++) {
        int32_t subtype = collision_events->entries[i].subtype;
        if (subtype < 0) {
          continue;
        }
        if (!bs_game_runner_contains_subtype(out_targets, count, subtype)) {
          if (count >= max_targets) {
            return count;
          }
          out_targets[count] = subtype;
          count++;
        }
      }
    }

    current = obj->parent_id;
    if (current < 0) {
      break;
    }
    depth++;
  }

  return count;
}

static void bs_game_runner_update_direction_from_path(bs_instance *instance,
                                                      double old_x,
                                                      double old_y,
                                                      double new_x,
                                                      double new_y) {
  const double pi = 3.14159265358979323846;
  double dx = 0.0;
  double dy = 0.0;
  if (instance == NULL) {
    return;
  }
  dx = new_x - old_x;
  dy = new_y - old_y;
  if (dx != 0.0 || dy != 0.0) {
    instance->direction = fmod((atan2(-dy, dx) * (180.0 / pi)) + 360.0, 360.0);
  }
}

static double bs_game_runner_compute_path_length(const bs_game_runner *runner, int32_t path_index) {
  const bs_path_data *path = NULL;
  size_t seg_count = 0;
  double total = 0.0;
  if (runner == NULL || runner->game_data == NULL) {
    return 0.0;
  }
  if (path_index < 0 || (size_t)path_index >= runner->game_data->path_count) {
    return 0.0;
  }
  path = &runner->game_data->paths[(size_t)path_index];
  if (path->point_count < 2) {
    return 0.0;
  }

  seg_count = path->is_closed ? path->point_count : (path->point_count - 1u);
  for (size_t i = 0; i < seg_count; i++) {
    const bs_path_point_data *p1 = &path->points[i];
    const bs_path_point_data *p2 = &path->points[(i + 1u) % path->point_count];
    double dx = (double)p2->x - (double)p1->x;
    double dy = (double)p2->y - (double)p1->y;
    total += sqrt((dx * dx) + (dy * dy));
  }
  return total;
}

static bool bs_game_runner_interpolate_path_position(const bs_game_runner *runner,
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
  if (out_x == NULL || out_y == NULL || runner == NULL || runner->game_data == NULL) {
    return false;
  }
  if (path_index < 0 || (size_t)path_index >= runner->game_data->path_count) {
    return false;
  }
  path = &runner->game_data->paths[(size_t)path_index];
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

static void bs_game_runner_update_path_following(bs_game_runner *runner) {
  const double pi = 3.14159265358979323846;
  if (runner == NULL) {
    return;
  }

  for (size_t i = 0; i < runner->instance_count; i++) {
    bs_instance *inst = &runner->instances[i];
    double path_length = 0.0;
    double old_x = 0.0;
    double old_y = 0.0;
    if (inst->destroyed || inst->path_index < 0 || inst->path_speed == 0.0) {
      continue;
    }

    path_length = bs_game_runner_compute_path_length(runner, inst->path_index);
    if (path_length <= 0.0) {
      continue;
    }

    old_x = inst->x;
    old_y = inst->y;
    inst->path_position += inst->path_speed / path_length;

    if (inst->path_position >= 1.0) {
      switch (inst->path_end_action) {
        case 0: {
          double new_x = 0.0;
          double new_y = 0.0;
          inst->path_position = 1.0;
          inst->path_speed = 0.0;
          if (bs_game_runner_interpolate_path_position(runner, inst->path_index, 1.0, &new_x, &new_y)) {
            new_x += inst->path_x_offset;
            new_y += inst->path_y_offset;
            bs_game_runner_update_direction_from_path(inst, old_x, old_y, new_x, new_y);
            inst->x = new_x;
            inst->y = new_y;
          }
          continue;
        }
        case 1:
          inst->path_position -= 1.0;
          break;
        case 2: {
          double new_x = 0.0;
          double new_y = 0.0;
          double speed = fabs(inst->path_speed);
          inst->path_position = 1.0;
          if (bs_game_runner_interpolate_path_position(runner, inst->path_index, 1.0, &new_x, &new_y)) {
            new_x += inst->path_x_offset;
            new_y += inst->path_y_offset;
            bs_game_runner_update_direction_from_path(inst, old_x, old_y, new_x, new_y);
            inst->x = new_x;
            inst->y = new_y;
          }
          inst->speed = speed;
          inst->hspeed = speed * cos(inst->direction * (pi / 180.0));
          inst->vspeed = -speed * sin(inst->direction * (pi / 180.0));
          bs_game_runner_path_end_instance(runner, inst);
          continue;
        }
        case 3:
          inst->path_position = 1.0 - (inst->path_position - 1.0);
          inst->path_speed = -inst->path_speed;
          break;
        default:
          break;
      }
    } else if (inst->path_position <= 0.0) {
      switch (inst->path_end_action) {
        case 0: {
          double new_x = 0.0;
          double new_y = 0.0;
          inst->path_position = 0.0;
          inst->path_speed = 0.0;
          if (bs_game_runner_interpolate_path_position(runner, inst->path_index, 0.0, &new_x, &new_y)) {
            new_x += inst->path_x_offset;
            new_y += inst->path_y_offset;
            bs_game_runner_update_direction_from_path(inst, old_x, old_y, new_x, new_y);
            inst->x = new_x;
            inst->y = new_y;
          }
          continue;
        }
        case 1:
          inst->path_position += 1.0;
          break;
        case 2: {
          double new_x = 0.0;
          double new_y = 0.0;
          double speed = fabs(inst->path_speed);
          inst->path_position = 0.0;
          if (bs_game_runner_interpolate_path_position(runner, inst->path_index, 0.0, &new_x, &new_y)) {
            new_x += inst->path_x_offset;
            new_y += inst->path_y_offset;
            bs_game_runner_update_direction_from_path(inst, old_x, old_y, new_x, new_y);
            inst->x = new_x;
            inst->y = new_y;
          }
          inst->speed = speed;
          inst->hspeed = speed * cos(inst->direction * (pi / 180.0));
          inst->vspeed = -speed * sin(inst->direction * (pi / 180.0));
          bs_game_runner_path_end_instance(runner, inst);
          continue;
        }
        case 3:
          inst->path_position = -inst->path_position;
          inst->path_speed = -inst->path_speed;
          break;
        default:
          break;
      }
    }

    {
      double new_x = 0.0;
      double new_y = 0.0;
      if (inst->path_index >= 0 &&
          bs_game_runner_interpolate_path_position(runner, inst->path_index, inst->path_position, &new_x, &new_y)) {
        new_x += inst->path_x_offset;
        new_y += inst->path_y_offset;
        bs_game_runner_update_direction_from_path(inst, old_x, old_y, new_x, new_y);
        inst->x = new_x;
        inst->y = new_y;
      }
    }
  }
}

static void bs_game_runner_dispatch_collision_events(bs_game_runner *runner) {
  int32_t *snapshot_ids = NULL;
  size_t snapshot_count = 0;
  if (runner == NULL) {
    return;
  }

  for (size_t i = 0; i < runner->instance_count; i++) {
    if (!runner->instances[i].destroyed) {
      snapshot_count++;
    }
  }
  if (snapshot_count == 0) {
    return;
  }

  snapshot_ids = (int32_t *)malloc(snapshot_count * sizeof(int32_t));
  if (snapshot_ids == NULL) {
    return;
  }
  {
    size_t at = 0;
    for (size_t i = 0; i < runner->instance_count; i++) {
      if (!runner->instances[i].destroyed) {
        snapshot_ids[at] = runner->instances[i].id;
        at++;
      }
    }
  }

  for (size_t i = 0; i < snapshot_count; i++) {
    bs_instance *inst = bs_game_runner_find_instance_by_id(runner, snapshot_ids[i]);
    bs_bbox inst_bbox = {0};
    int32_t targets[256];
    size_t target_count = 0;
    if (inst == NULL || inst->destroyed) {
      continue;
    }
    if (!bs_game_runner_compute_instance_bbox(runner, inst, &inst_bbox)) {
      continue;
    }

    target_count = bs_game_runner_collect_collision_targets(runner,
                                                            inst->object_index,
                                                            targets,
                                                            sizeof(targets) / sizeof(targets[0]));
    for (size_t t = 0; t < target_count; t++) {
      int32_t target_obj = targets[t];
      if (inst->destroyed) {
        break;
      }

      for (size_t j = 0; j < snapshot_count; j++) {
        bs_instance *other = NULL;
        if (snapshot_ids[j] == inst->id) {
          continue;
        }
        other = bs_game_runner_find_instance_by_id(runner, snapshot_ids[j]);
        if (other == NULL || other->destroyed) {
          continue;
        }
        if (!bs_game_runner_object_is_child_of(runner, other->object_index, target_obj)) {
          continue;
        }
        if (!bs_game_runner_instances_overlap(runner, inst, other)) {
          continue;
        }

        if (other->solid) {
          inst->x = inst->xprevious;
          inst->y = inst->yprevious;
          if (!bs_game_runner_compute_instance_bbox(runner, inst, &inst_bbox)) {
            break;
          }
        }
        if (inst->solid) {
          other->x = other->xprevious;
          other->y = other->yprevious;
        }

        bs_game_runner_fire_event(runner, inst, BS_EVENT_COLLISION, target_obj, other);
        break;
      }

      if (!bs_game_runner_compute_instance_bbox(runner, inst, &inst_bbox)) {
        break;
      }
    }
  }

  free(snapshot_ids);
}

static void bs_game_runner_resolve_solid_overlaps(bs_game_runner *runner) {
  if (runner == NULL) {
    return;
  }

  for (size_t i = 0; i < runner->instance_count; i++) {
    bs_instance *inst = &runner->instances[i];
    bs_bbox inst_bbox = {0};
    if (inst->destroyed) {
      continue;
    }
    if (inst->x == inst->xprevious && inst->y == inst->yprevious) {
      continue;
    }
    if (!bs_game_runner_compute_instance_bbox(runner, inst, &inst_bbox)) {
      continue;
    }

    for (size_t j = 0; j < runner->instance_count; j++) {
      bs_instance *other = &runner->instances[j];
      if (other->destroyed || other == inst || !other->solid) {
        continue;
      }
      if (bs_game_runner_instances_overlap(runner, inst, other)) {
        inst->x = inst->xprevious;
        inst->y = inst->yprevious;
        break;
      }
    }
  }
}

static void bs_game_runner_check_outside_room_events(bs_game_runner *runner) {
  double room_w = 0.0;
  double room_h = 0.0;
  if (runner == NULL || runner->current_room == NULL) {
    return;
  }

  room_w = (double)runner->current_room->width;
  room_h = (double)runner->current_room->height;
  for (size_t i = 0; i < runner->instance_count; i++) {
    bs_instance *inst = &runner->instances[i];
    bs_bbox bbox = {0};
    bool outside = false;
    if (inst->destroyed) {
      continue;
    }

    if (bs_game_runner_compute_instance_bbox(runner, inst, &bbox)) {
      outside = (bbox.right < 0.0 || bbox.left > room_w || bbox.bottom < 0.0 || bbox.top > room_h);
    } else {
      outside = (inst->x < 0.0 || inst->x > room_w || inst->y < 0.0 || inst->y > room_h);
    }

    if (outside) {
      if (!inst->has_been_marked_as_outside_room) {
        inst->has_been_marked_as_outside_room = true;
        bs_game_runner_fire_event(runner, inst, BS_EVENT_OTHER, BS_OTHER_OUTSIDE_ROOM, NULL);
      }
    } else {
      inst->has_been_marked_as_outside_room = false;
    }
  }
}

static void bs_game_runner_dispatch_draw_events_for_instance(bs_game_runner *runner, bs_instance *instance) {
  int32_t seen_subtypes[64];
  size_t seen_count = 0;
  int32_t current_object = 0;
  int depth = 0;
  bool has_any_draw_event = false;
  if (runner == NULL || instance == NULL || instance->destroyed || runner->game_data == NULL) {
    return;
  }

  current_object = instance->object_index;
  while (depth < 64) {
    const bs_game_object_data *obj = NULL;
    const bs_object_event_list *draw_list = NULL;
    if (current_object < 0 || (size_t)current_object >= runner->game_data->object_count) {
      break;
    }
    obj = &runner->game_data->objects[(size_t)current_object];
    if ((size_t)BS_EVENT_DRAW < obj->event_type_count) {
      draw_list = &obj->events[(size_t)BS_EVENT_DRAW];
      if (draw_list->entry_count > 0) {
        has_any_draw_event = true;
      }
      for (size_t i = 0; i < draw_list->entry_count; i++) {
        int32_t subtype = draw_list->entries[i].subtype;
        if (!bs_game_runner_contains_subtype(seen_subtypes, seen_count, subtype)) {
          bs_game_runner_fire_event(runner, instance, BS_EVENT_DRAW, subtype, NULL);
          if (seen_count < (sizeof(seen_subtypes) / sizeof(seen_subtypes[0]))) {
            seen_subtypes[seen_count] = subtype;
            seen_count++;
          }
        }
      }
    }
    current_object = obj->parent_id;
    if (current_object < 0) {
      break;
    }
    depth++;
  }

  if (!has_any_draw_event &&
      instance->visible &&
      instance->sprite_index >= 0 &&
      (runner->render.draw_sprite_ext != NULL || runner->render.draw_sprite != NULL)) {
    int32_t draw_frame = 0;
    if (instance->image_single >= 0.0) {
      draw_frame = (int32_t)instance->image_single;
    } else {
      draw_frame = (int32_t)floor(instance->image_index);
    }
    if (runner->render.draw_sprite_ext != NULL) {
      runner->render.draw_sprite_ext(runner->render.userdata,
                                     runner,
                                     instance->sprite_index,
                                     draw_frame,
                                     instance->x,
                                     instance->y,
                                     instance->image_xscale,
                                     instance->image_yscale,
                                     instance->image_angle,
                                     instance->image_blend,
                                     instance->image_alpha);
    } else {
      runner->render.draw_sprite(runner->render.userdata,
                                 runner,
                                 instance->sprite_index,
                                 draw_frame,
                                 instance->x,
                                 instance->y,
                                 instance->image_blend,
                                 instance->image_alpha);
    }
  }
}

static int32_t bs_game_runner_background_tpag_index(const bs_game_runner *runner, int32_t bg_def_index) {
  if (runner == NULL || runner->game_data == NULL) {
    return -1;
  }
  if (bg_def_index < 0 || (size_t)bg_def_index >= runner->game_data->background_count) {
    return -1;
  }
  return runner->game_data->backgrounds[(size_t)bg_def_index].tpag_index;
}

static bool bs_game_runner_depth_seen(const int32_t *depths, size_t count, int32_t depth) {
  for (size_t i = 0; i < count; i++) {
    if (depths[i] == depth) {
      return true;
    }
  }
  return false;
}

static void bs_game_runner_draw_room_backgrounds(bs_game_runner *runner, bool foreground) {
  const bs_room_data *room = NULL;
  if (runner == NULL || runner->render.draw_background == NULL) {
    return;
  }
  room = runner->current_room;
  if (room == NULL) {
    return;
  }
  for (size_t i = 0; i < room->background_count; i++) {
    const bs_room_background_data *bg = &room->backgrounds[i];
    int32_t tpag_index = -1;
    if (!bg->enabled || bg->foreground != foreground || bg->bg_def_index < 0) {
      continue;
    }
    tpag_index = bs_game_runner_background_tpag_index(runner, bg->bg_def_index);
    if (tpag_index < 0) {
      continue;
    }
    runner->render.draw_background(runner->render.userdata,
                                   runner,
                                   tpag_index,
                                   bg->x,
                                   bg->y,
                                   bg->tile_x,
                                   bg->tile_y);
  }
}

static void bs_game_runner_dispatch_draw_events_all(bs_game_runner *runner) {
  const bs_room_data *room = NULL;
  int32_t *depths = NULL;
  size_t depth_count = 0;
  size_t depth_capacity = 0;
  if (runner == NULL) {
    return;
  }
  room = runner->current_room;

  depth_capacity = runner->instance_count + (room != NULL ? room->tile_count : 0);
  if (depth_capacity > 0) {
    depths = (int32_t *)malloc(depth_capacity * sizeof(int32_t));
    if (depths == NULL) {
      return;
    }
  }

  for (size_t i = 0; i < runner->instance_count; i++) {
    if (!runner->instances[i].destroyed &&
        !bs_game_runner_depth_seen(depths, depth_count, runner->instances[i].depth)) {
      depths[depth_count++] = runner->instances[i].depth;
    }
  }
  if (room != NULL) {
    for (size_t i = 0; i < room->tile_count; i++) {
      int32_t depth = room->tiles[i].depth;
      if (!bs_game_runner_depth_seen(depths, depth_count, depth)) {
        depths[depth_count++] = depth;
      }
    }
  }

  for (size_t i = 0; i < depth_count; i++) {
    for (size_t j = i + 1; j < depth_count; j++) {
      if (depths[i] < depths[j]) {
        int32_t tmp = depths[i];
        depths[i] = depths[j];
        depths[j] = tmp;
      }
    }
  }

  for (size_t d = 0; d < depth_count; d++) {
    int32_t depth = depths[d];
    if (room != NULL && runner->render.draw_tile != NULL) {
      for (size_t i = 0; i < room->tile_count; i++) {
        const bs_room_tile_data *tile = &room->tiles[i];
        int32_t tpag_index = -1;
        if (tile->depth != depth) {
          continue;
        }
        tpag_index = bs_game_runner_background_tpag_index(runner, tile->bg_def_index);
        if (tpag_index < 0) {
          continue;
        }
        runner->render.draw_tile(runner->render.userdata,
                                 runner,
                                 tpag_index,
                                 tile->x,
                                 tile->y,
                                 tile->source_x,
                                 tile->source_y,
                                 tile->width,
                                 tile->height,
                                 tile->scale_x,
                                 tile->scale_y,
                                 (int32_t)tile->color);
      }
    }

    for (size_t i = 0; i < runner->instance_count; i++) {
      if (!runner->instances[i].destroyed && runner->instances[i].depth == depth) {
        bs_game_runner_dispatch_draw_events_for_instance(runner, &runner->instances[i]);
      }
    }
  }

  free(depths);
}

static void bs_game_runner_advance_instance_animation(bs_game_runner *runner, bs_instance *instance) {
  size_t image_number = 0;
  if (runner == NULL || instance == NULL || instance->destroyed) {
    return;
  }

  if (instance->sprite_index < 0) {
    instance->image_index = 0.0;
    return;
  }

  if (instance->image_single >= 0.0) {
    return;
  }

  image_number = bs_game_runner_sprite_frame_count(runner, instance->sprite_index);
  if (image_number <= 1u) {
    instance->image_index = 0.0;
    return;
  }

  if (!isfinite(instance->image_index)) {
    instance->image_index = 0.0;
  }
  if (!isfinite(instance->image_speed)) {
    instance->image_speed = 0.0;
  }

  instance->image_index += instance->image_speed;
  while (instance->image_index >= (double)image_number) {
    instance->image_index -= (double)image_number;
    bs_game_runner_fire_event(runner, instance, BS_EVENT_OTHER, BS_OTHER_ANIMATION_END, NULL);
    if (instance->destroyed) {
      return;
    }
  }
  while (instance->image_index < 0.0) {
    instance->image_index += (double)image_number;
    bs_game_runner_fire_event(runner, instance, BS_EVENT_OTHER, BS_OTHER_ANIMATION_END, NULL);
    if (instance->destroyed) {
      return;
    }
  }
}

static int32_t bs_game_runner_clamp_i32(int32_t value, int32_t min_value, int32_t max_value) {
  if (value < min_value) {
    return min_value;
  }
  if (value > max_value) {
    return max_value;
  }
  return value;
}

static bs_instance *bs_game_runner_find_view_follow_instance(bs_game_runner *runner, int32_t follow_object_id) {
  if (runner == NULL || runner->game_data == NULL || follow_object_id < 0) {
    return NULL;
  }
  if (follow_object_id >= 100000) {
    return bs_game_runner_find_instance_by_id(runner, follow_object_id);
  }
  for (size_t i = 0; i < runner->instance_count; i++) {
    bs_instance *inst = &runner->instances[i];
    if (inst->destroyed) {
      continue;
    }
    if (inst->id == follow_object_id) {
      return inst;
    }
    if (bs_game_runner_object_is_child_of(runner, inst->object_index, follow_object_id)) {
      return inst;
    }
  }
  return NULL;
}

static void bs_game_runner_update_views(bs_game_runner *runner) {
  int32_t room_w = 640;
  int32_t room_h = 480;
  if (runner == NULL || runner->current_room == NULL || runner->current_room->views == NULL) {
    return;
  }

  room_w = runner->current_room->width > 0
               ? runner->current_room->width
               : (runner->surface_width > 0 ? runner->surface_width : 640);
  room_h = runner->current_room->height > 0
               ? runner->current_room->height
               : (runner->surface_height > 0 ? runner->surface_height : 480);

  for (size_t i = 0; i < runner->current_room->view_count; i++) {
    bs_room_view_data *view = &runner->current_room->views[i];
    bs_instance *target = NULL;
    int32_t view_w = 0;
    int32_t view_h = 0;
    int32_t border_h = 0;
    int32_t border_v = 0;
    int32_t desired_x = 0;
    int32_t desired_y = 0;
    int32_t max_x = 0;
    int32_t max_y = 0;
    if (!view->enabled) {
      continue;
    }

    target = bs_game_runner_find_view_follow_instance(runner, view->follow_object_id);
    if (target == NULL) {
      continue;
    }

    view_w = view->view_w > 0 ? view->view_w : (runner->surface_width > 0 ? runner->surface_width : 640);
    view_h = view->view_h > 0 ? view->view_h : (runner->surface_height > 0 ? runner->surface_height : 480);
    border_h = view->border_h > 0 ? view->border_h : 0;
    border_v = view->border_v > 0 ? view->border_v : 0;
    if (border_h > view_w / 2) {
      border_h = view_w / 2;
    }
    if (border_v > view_h / 2) {
      border_v = view_h / 2;
    }

    desired_x = view->view_x;
    desired_y = view->view_y;
    if (target->x < (double)view->view_x + (double)border_h) {
      desired_x = (int32_t)floor(target->x - (double)border_h);
    } else if (target->x > (double)view->view_x + (double)(view_w - border_h)) {
      desired_x = (int32_t)floor(target->x + (double)border_h - (double)view_w);
    }
    if (target->y < (double)view->view_y + (double)border_v) {
      desired_y = (int32_t)floor(target->y - (double)border_v);
    } else if (target->y > (double)view->view_y + (double)(view_h - border_v)) {
      desired_y = (int32_t)floor(target->y + (double)border_v - (double)view_h);
    }

    max_x = room_w - view_w;
    max_y = room_h - view_h;
    if (max_x < 0) {
      max_x = 0;
    }
    if (max_y < 0) {
      max_y = 0;
    }
    desired_x = bs_game_runner_clamp_i32(desired_x, 0, max_x);
    desired_y = bs_game_runner_clamp_i32(desired_y, 0, max_y);

    if (view->speed_h < 0) {
      view->view_x = desired_x;
    } else if (view->speed_h > 0) {
      if (view->view_x < desired_x) {
        int32_t step = desired_x - view->view_x;
        if (step > view->speed_h) {
          step = view->speed_h;
        }
        view->view_x += step;
      } else if (view->view_x > desired_x) {
        int32_t step = view->view_x - desired_x;
        if (step > view->speed_h) {
          step = view->speed_h;
        }
        view->view_x -= step;
      }
    }

    if (view->speed_v < 0) {
      view->view_y = desired_y;
    } else if (view->speed_v > 0) {
      if (view->view_y < desired_y) {
        int32_t step = desired_y - view->view_y;
        if (step > view->speed_v) {
          step = view->speed_v;
        }
        view->view_y += step;
      } else if (view->view_y > desired_y) {
        int32_t step = view->view_y - desired_y;
        if (step > view->speed_v) {
          step = view->speed_v;
        }
        view->view_y -= step;
      }
    }
  }
}

static void bs_game_runner_dispatch_key_event(bs_game_runner *runner, int32_t event_type, int32_t key) {
  if (runner == NULL || key < 0 || key >= 256) {
    return;
  }
  bs_game_runner_dispatch_event_all(runner, event_type, key);
}

static void bs_game_runner_save_room_state(bs_game_runner *runner, int32_t room_index) {
  bs_saved_room_state *state = NULL;
  size_t count = 0;
  size_t write = 0;
  if (runner == NULL ||
      room_index < 0 ||
      (size_t)room_index >= runner->saved_room_state_count ||
      runner->saved_room_states == NULL) {
    return;
  }

  state = &runner->saved_room_states[(size_t)room_index];
  bs_saved_room_state_clear(state);

  for (size_t i = 0; i < runner->instance_count; i++) {
    const bs_instance *inst = &runner->instances[i];
    if (!inst->destroyed && !inst->persistent) {
      count++;
    }
  }

  if (count == 0) {
    return;
  }

  state->instances = (bs_instance *)calloc(count, sizeof(bs_instance));
  if (state->instances == NULL) {
    state->instance_count = 0;
    return;
  }

  for (size_t i = 0; i < runner->instance_count; i++) {
    const bs_instance *inst = &runner->instances[i];
    if (inst->destroyed || inst->persistent) {
      continue;
    }
    if (!bs_instance_clone(inst, &state->instances[write])) {
      for (size_t j = 0; j < write; j++) {
        bs_instance_dispose(&state->instances[j]);
      }
      free(state->instances);
      state->instances = NULL;
      state->instance_count = 0;
      return;
    }
    write++;
  }
  state->instance_count = write;
}

static bool bs_game_runner_restore_room_state(bs_game_runner *runner, int32_t room_index) {
  bs_saved_room_state *state = NULL;
  if (runner == NULL ||
      room_index < 0 ||
      (size_t)room_index >= runner->saved_room_state_count ||
      runner->saved_room_states == NULL) {
    return false;
  }

  state = &runner->saved_room_states[(size_t)room_index];
  if (state->instance_count == 0 || state->instances == NULL) {
    return false;
  }
  if (!bs_game_runner_ensure_instance_capacity(runner, runner->instance_count + state->instance_count)) {
    return false;
  }

  for (size_t i = 0; i < state->instance_count; i++) {
    bs_instance *dst = &runner->instances[runner->instance_count];
    *dst = state->instances[i];
    dst->destroyed = false;
    runner->instance_count++;
    memset(&state->instances[i], 0, sizeof(state->instances[i]));
  }

  free(state->instances);
  state->instances = NULL;
  state->instance_count = 0;
  return true;
}

void bs_game_runner_goto_room(bs_game_runner *runner, int32_t room_index) {
  const bs_room_data *room = NULL;
  int32_t *created_ids = NULL;
  size_t created_id_capacity = 0;
  size_t created_id_count = 0;
  size_t created = 0;
  size_t invalid_object_count = 0;
  uint64_t calls_before_create = 0;
  uint64_t instr_before_create = 0;
  int32_t leaving_room_index = -1;
  bool restored_persistent_room = false;

  if (runner == NULL || runner->game_data == NULL || runner->vm == NULL) {
    return;
  }
  if (room_index < 0 || (size_t)room_index >= runner->game_data->room_count) {
    printf("WARNING: room_goto ignored (invalid room index %d)\n", room_index);
    return;
  }

  if (runner->current_room_index >= 0) {
    bs_game_runner_dispatch_event_all(runner, BS_EVENT_OTHER, BS_OTHER_ROOM_END);
  }
  leaving_room_index = runner->current_room_index;

  if (leaving_room_index >= 0 &&
      (size_t)leaving_room_index < runner->room_persistent_flag_count &&
      runner->room_persistent_flags != NULL &&
      runner->room_persistent_flags[(size_t)leaving_room_index]) {
    bs_game_runner_save_room_state(runner, leaving_room_index);
  }

  room = &runner->game_data->rooms[(size_t)room_index];
  {
    size_t write_index = 0;
    for (size_t i = 0; i < runner->instance_count; i++) {
      bs_instance *inst = &runner->instances[i];
      bool keep_instance = (!inst->destroyed && inst->persistent);
      if (keep_instance) {
        if (write_index != i) {
          runner->instances[write_index] = runner->instances[i];
          memset(&runner->instances[i], 0, sizeof(runner->instances[i]));
        }
        write_index++;
      } else {
        bs_instance_dispose(inst);
      }
    }
    runner->instance_count = write_index;
  }
  runner->current_room_index = room_index;
  runner->current_room = room;
  runner->pending_room_goto = -1;
  restored_persistent_room = bs_game_runner_restore_room_state(runner, room_index);

  calls_before_create = runner->total_vm_event_calls;
  instr_before_create = runner->total_vm_instructions;
  if (!restored_persistent_room) {
    created_id_capacity = room->instance_count;
    if (created_id_capacity > 0) {
      created_ids = (int32_t *)malloc(created_id_capacity * sizeof(int32_t));
    }

    for (size_t i = 0; i < room->instance_count; i++) {
      const bs_room_instance_data *room_inst = &room->instances[i];
      bs_instance *created_instance = NULL;
      if (room_inst->object_def_id < 0 || (size_t)room_inst->object_def_id >= runner->game_data->object_count) {
        invalid_object_count++;
        continue;
      }

      created_instance = bs_game_runner_create_instance(runner,
                                                        room_inst->object_def_id,
                                                        (double)room_inst->x,
                                                        (double)room_inst->y,
                                                        room_inst->instance_id);
      if (created_instance != NULL) {
        created++;
        if (created_ids != NULL && created_id_count < created_id_capacity) {
          created_ids[created_id_count] = created_instance->id;
          created_id_count++;
        }

        if (room_inst->creation_code_id >= 0 &&
            (size_t)room_inst->creation_code_id < runner->game_data->code_entry_count) {
          bs_vm_execute_result inst_create_result = {0};
          (void)bs_vm_execute_code(runner->vm,
                                   (size_t)room_inst->creation_code_id,
                                   120000u,
                                   false,
                                   &inst_create_result);
        }
      }
    }

    printf("Room enter: id=%d name=%s instances=%zu invalid_obj_refs=%zu\n",
           room_index,
           room->name != NULL ? room->name : "<unnamed>",
           created,
           invalid_object_count);
  } else {
    printf("Room restore: id=%d name=%s restored_instances=%zu\n",
           room_index,
           room->name != NULL ? room->name : "<unnamed>",
           runner->instance_count);
  }

  if (runner->trace_events && room_index == 1) {
    for (size_t i = 0; i < runner->instance_count; i++) {
      const bs_instance *inst = &runner->instances[i];
      if (inst->object_index >= 0 && (size_t)inst->object_index < runner->game_data->object_count) {
        const bs_game_object_data *obj = &runner->game_data->objects[(size_t)inst->object_index];
        printf("  [OBJ EVT] obj=%d name=%s\n",
               inst->object_index,
               obj->name != NULL ? obj->name : "<unnamed>");
        for (size_t evt = 0; evt < obj->event_type_count; evt++) {
          const bs_object_event_list *list = &obj->events[evt];
          if (list->entry_count == 0) {
            continue;
          }
          printf("    type=%zu subtypes=", evt);
          for (size_t e = 0; e < list->entry_count; e++) {
            printf("%d%s", list->entries[e].subtype, (e + 1 < list->entry_count) ? "," : "");
          }
          printf("\n");
        }
      }
    }
  }

  if (!restored_persistent_room) {
    if (room->creation_code_id >= 0 && (size_t)room->creation_code_id < runner->game_data->code_entry_count) {
      bs_vm_execute_result room_create_result = {0};
      const bs_code_entry_data *entry = &runner->game_data->code_entries[(size_t)room->creation_code_id];
      bool ok = bs_vm_execute_code(runner->vm, (size_t)room->creation_code_id, 180000u, false, &room_create_result);
      printf("  Room creation code: id=%d name=%s ok=%s reason=%s instructions=%u\n",
             room->creation_code_id,
             entry->name != NULL ? entry->name : "<unnamed>",
             ok ? "true" : "false",
             bs_vm_exit_reason_to_string(room_create_result.exit_reason),
             (unsigned)room_create_result.instructions_executed);
    }
    for (size_t i = 0; i < created_id_count; i++) {
      bs_instance *created_instance = bs_game_runner_find_instance_by_id(runner, created_ids[i]);
      if (created_instance != NULL && !created_instance->destroyed) {
        bs_game_runner_fire_event(runner, created_instance, BS_EVENT_CREATE, 0, NULL);
      }
    }
  }
  free(created_ids);
  created_ids = NULL;

  if (!runner->game_started) {
    runner->game_started = true;
    bs_game_runner_dispatch_event_all(runner, BS_EVENT_OTHER, BS_OTHER_GAME_START);
  }

  bs_game_runner_dispatch_event_all(runner, BS_EVENT_OTHER, BS_OTHER_ROOM_START);

  printf("  Room setup events executed: calls=%llu instructions=%llu\n",
         (unsigned long long)(runner->total_vm_event_calls - calls_before_create),
         (unsigned long long)(runner->total_vm_instructions - instr_before_create));
}

void bs_game_runner_init(bs_game_runner *runner, const bs_game_data *game_data, bs_vm *vm) {
  int32_t first_room = 0;
  if (runner == NULL) {
    return;
  }

  memset(runner, 0, sizeof(*runner));
  runner->game_data = game_data;
  runner->vm = vm;
  if (vm != NULL) {
    vm->runner = runner;
    vm->current_self_id = -4;
    vm->current_other_id = -4;
  }
  runner->initialized = true;
  runner->frame_count = 0;
  runner->should_quit = false;
  runner->current_room_index = -1;
  runner->current_room = NULL;
  runner->pending_room_goto = -1;
  runner->next_instance_id = 100000;
  runner->instances = NULL;
  runner->instance_count = 0;
  runner->instance_capacity = 0;
  runner->room_persistent_flags = NULL;
  runner->room_persistent_flag_count = 0;
  runner->saved_room_states = NULL;
  runner->saved_room_state_count = 0;
  memset(runner->keys_held, 0, sizeof(runner->keys_held));
  memset(runner->keys_pressed, 0, sizeof(runner->keys_pressed));
  memset(runner->keys_released, 0, sizeof(runner->keys_released));
  runner->keyboard_key = 0;
  runner->keyboard_lastkey = 0;
  runner->total_vm_event_calls = 0;
  runner->total_vm_instructions = 0;
  runner->game_started = false;
  runner->trace_events = false;
  runner->event_context_active = false;
  runner->current_event_type = -1;
  runner->current_event_subtype = -1;
  runner->current_event_object_index = -1;
  runner->draw_color = 0x00FFFFFF;
  runner->draw_font_index = -1;
  runner->draw_alpha = 255;
  runner->draw_halign = 0;
  runner->draw_valign = 0;
  runner->image_blend = 0x00FFFFFF;
  runner->image_alpha = 255;
  runner->surface_width = (game_data != NULL && game_data->gen8.window_width > 0)
                              ? (int32_t)game_data->gen8.window_width
                              : 640;
  runner->surface_height = (game_data != NULL && game_data->gen8.window_height > 0)
                               ? (int32_t)game_data->gen8.window_height
                               : 480;
  runner->render.userdata = NULL;
  runner->render.clear = NULL;
  runner->render.draw_sprite = NULL;
  runner->render.draw_sprite_ext = NULL;
  runner->render.draw_sprite_part_ext = NULL;
  runner->render.draw_background = NULL;
  runner->render.draw_tile = NULL;
  runner->render.draw_text = NULL;
  runner->render.draw_rect = NULL;
  memset(&runner->audio, 0, sizeof(runner->audio));
  if (game_data != NULL && game_data->room_count > 0) {
    runner->room_persistent_flags = (bool *)calloc(game_data->room_count, sizeof(bool));
    runner->saved_room_states = (bs_saved_room_state *)calloc(game_data->room_count, sizeof(bs_saved_room_state));
    if (runner->room_persistent_flags != NULL && runner->saved_room_states != NULL) {
      runner->room_persistent_flag_count = game_data->room_count;
      runner->saved_room_state_count = game_data->room_count;
      for (size_t i = 0; i < game_data->room_count; i++) {
        runner->room_persistent_flags[i] = game_data->rooms[i].persistent;
      }
    } else {
      free(runner->room_persistent_flags);
      free(runner->saved_room_states);
      runner->room_persistent_flags = NULL;
      runner->saved_room_states = NULL;
      runner->room_persistent_flag_count = 0;
      runner->saved_room_state_count = 0;
    }
  }
  {
    const char *trace_events_env = getenv("BS_TRACE_EVENTS");
    if (trace_events_env != NULL &&
        (strcmp(trace_events_env, "1") == 0 || strcmp(trace_events_env, "true") == 0)) {
      runner->trace_events = true;
    }
  }

  if (game_data != NULL) {
    if (game_data->gen8.room_order_count > 0) {
      first_room = (int32_t)game_data->gen8.room_order[0];
    }
    if (first_room < 0 || (size_t)first_room >= game_data->room_count) {
      first_room = 0;
    }
  }

  printf("GameRunner initialized (runtime bootstrap)\n");
  bs_game_runner_goto_room(runner, first_room);
}

void bs_game_runner_step(bs_game_runner *runner) {
  uint64_t calls_before = 0;
  uint64_t instructions_before = 0;
  const double pi = 3.14159265358979323846;
  const bool trace_frame = bs_trace_frame_enabled();

  if (runner == NULL || !runner->initialized || runner->game_data == NULL) {
    return;
  }

  runner->frame_count++;
  if (trace_frame) {
    printf("Frame %llu room=%d instances=%zu\n",
           (unsigned long long)runner->frame_count,
           runner->current_room_index,
           runner->instance_count);
  }

  if (runner->pending_room_goto >= 0) {
    bs_game_runner_goto_room(runner, runner->pending_room_goto);
  }

  for (size_t i = 0; i < runner->instance_count; i++) {
    if (runner->instances[i].destroyed) {
      continue;
    }
    runner->instances[i].xprevious = runner->instances[i].x;
    runner->instances[i].yprevious = runner->instances[i].y;
  }

  calls_before = runner->total_vm_event_calls;
  instructions_before = runner->total_vm_instructions;

  bs_game_runner_dispatch_event_all(runner, BS_EVENT_STEP, 1);

  for (size_t i = 0; i < runner->instance_count; i++) {
    bs_instance *inst = &runner->instances[i];
    if (inst->destroyed) {
      continue;
    }
    for (int alarm_idx = 0; alarm_idx < 12; alarm_idx++) {
      if (inst->alarm[alarm_idx] >= 0) {
        inst->alarm[alarm_idx]--;
        if (inst->alarm[alarm_idx] == 0) {
          inst->alarm[alarm_idx] = -1;
          bs_game_runner_fire_event(runner, inst, BS_EVENT_ALARM, alarm_idx, NULL);
        }
      }
    }
  }

  for (int key = 0; key < 256; key++) {
    if (runner->keys_held[key]) {
      bs_game_runner_dispatch_key_event(runner, BS_EVENT_KEYBOARD, key);
    }
    if (runner->keys_pressed[key]) {
      bs_game_runner_dispatch_key_event(runner, BS_EVENT_KEYPRESS, key);
    }
    if (runner->keys_released[key]) {
      bs_game_runner_dispatch_key_event(runner, BS_EVENT_KEYRELEASE, key);
    }
  }

  bs_game_runner_dispatch_event_all(runner, BS_EVENT_STEP, 0);
  bs_game_runner_dispatch_collision_events(runner);
  bs_game_runner_resolve_solid_overlaps(runner);
  bs_game_runner_dispatch_event_all(runner, BS_EVENT_STEP, 2);
  bs_game_runner_update_path_following(runner);

  for (size_t i = 0; i < runner->instance_count; i++) {
    bs_instance *inst = &runner->instances[i];
    if (inst->destroyed) {
      continue;
    }

    if (inst->gravity != 0.0) {
      const double dir_rad = inst->gravity_direction * (pi / 180.0);
      inst->hspeed += inst->gravity * cos(dir_rad);
      inst->vspeed -= inst->gravity * sin(dir_rad);
      inst->speed = sqrt(inst->hspeed * inst->hspeed + inst->vspeed * inst->vspeed);
      inst->direction = fmod((atan2(-inst->vspeed, inst->hspeed) * (180.0 / pi)) + 360.0, 360.0);
    }

    if (inst->friction != 0.0 && inst->speed != 0.0) {
      const double new_speed = inst->speed - inst->friction;
      if (new_speed <= 0.0) {
        inst->speed = 0.0;
        inst->hspeed = 0.0;
        inst->vspeed = 0.0;
      } else {
        const double dir_rad = inst->direction * (pi / 180.0);
        inst->speed = new_speed;
        inst->hspeed = new_speed * cos(dir_rad);
        inst->vspeed = -new_speed * sin(dir_rad);
      }
    }

    if (inst->hspeed != 0.0 || inst->vspeed != 0.0) {
      inst->x += inst->hspeed;
      inst->y += inst->vspeed;
    }
  }

  bs_game_runner_check_outside_room_events(runner);

  for (size_t i = 0; i < runner->instance_count; i++) {
    bs_game_runner_advance_instance_animation(runner, &runner->instances[i]);
  }

  bs_game_runner_update_views(runner);

  if (runner->current_room != NULL && runner->current_room->draw_bg_color && runner->render.clear != NULL) {
    runner->render.clear(runner->render.userdata, (int32_t)runner->current_room->bg_color);
  }
  bs_game_runner_draw_room_backgrounds(runner, false);
  bs_game_runner_dispatch_draw_events_all(runner);
  bs_game_runner_draw_room_backgrounds(runner, true);

  {
    size_t write_index = 0;
    for (size_t i = 0; i < runner->instance_count; i++) {
      if (runner->instances[i].destroyed) {
        bs_instance_dispose(&runner->instances[i]);
        continue;
      }
      if (write_index != i) {
        runner->instances[write_index] = runner->instances[i];
        memset(&runner->instances[i], 0, sizeof(runner->instances[i]));
      }
      write_index++;
    }
    runner->instance_count = write_index;
  }

  bs_game_runner_trace_intro_state(runner);

  if (trace_frame) {
    printf("  Step VM: calls=%llu instructions=%llu\n",
           (unsigned long long)(runner->total_vm_event_calls - calls_before),
           (unsigned long long)(runner->total_vm_instructions - instructions_before));
  }

  memset(runner->keys_pressed, 0, sizeof(runner->keys_pressed));
  memset(runner->keys_released, 0, sizeof(runner->keys_released));
}

void bs_game_runner_on_key_down(bs_game_runner *runner, int32_t key) {
  if (runner == NULL || key < 0 || key >= 256) {
    return;
  }

  if (!runner->keys_held[key]) {
    runner->keys_pressed[key] = true;
  }
  runner->keys_held[key] = true;
  runner->keyboard_key = key;
  runner->keyboard_lastkey = key;
}

void bs_game_runner_on_key_up(bs_game_runner *runner, int32_t key) {
  if (runner == NULL || key < 0 || key >= 256) {
    return;
  }

  runner->keys_held[key] = false;
  runner->keys_released[key] = true;
}

void bs_game_runner_dispose(bs_game_runner *runner) {
  if (runner == NULL) {
    return;
  }

  if (runner->vm != NULL && runner->vm->runner == runner) {
    runner->vm->runner = NULL;
    runner->vm->current_self_id = -4;
    runner->vm->current_other_id = -4;
  }

  bs_game_runner_clear_instances(runner);
  bs_game_runner_clear_all_saved_room_states(runner);
  free(runner->saved_room_states);
  free(runner->room_persistent_flags);
  runner->initialized = false;
  runner->game_data = NULL;
  runner->vm = NULL;
  runner->frame_count = 0;
  runner->should_quit = false;
  runner->current_room_index = -1;
  runner->current_room = NULL;
  runner->pending_room_goto = -1;
  runner->next_instance_id = 100000;
  runner->saved_room_states = NULL;
  runner->saved_room_state_count = 0;
  runner->room_persistent_flags = NULL;
  runner->room_persistent_flag_count = 0;
  memset(runner->keys_held, 0, sizeof(runner->keys_held));
  memset(runner->keys_pressed, 0, sizeof(runner->keys_pressed));
  memset(runner->keys_released, 0, sizeof(runner->keys_released));
  runner->keyboard_key = 0;
  runner->keyboard_lastkey = 0;
  runner->total_vm_event_calls = 0;
  runner->total_vm_instructions = 0;
  runner->game_started = false;
  runner->trace_events = false;
  runner->event_context_active = false;
  runner->current_event_type = -1;
  runner->current_event_subtype = -1;
  runner->current_event_object_index = -1;
  runner->draw_color = 0x00FFFFFF;
  runner->draw_font_index = -1;
  runner->draw_alpha = 255;
  runner->draw_halign = 0;
  runner->draw_valign = 0;
  runner->image_blend = 0x00FFFFFF;
  runner->image_alpha = 255;
  runner->surface_width = 640;
  runner->surface_height = 480;
  runner->render.userdata = NULL;
  runner->render.clear = NULL;
  runner->render.draw_sprite = NULL;
  runner->render.draw_sprite_ext = NULL;
  runner->render.draw_sprite_part_ext = NULL;
  runner->render.draw_background = NULL;
  runner->render.draw_tile = NULL;
  runner->render.draw_text = NULL;
  runner->render.draw_rect = NULL;
  memset(&runner->audio, 0, sizeof(runner->audio));
}
