#ifndef BS_RUNTIME_GAME_RUNNER_H
#define BS_RUNTIME_GAME_RUNNER_H

#include "bs/common.h"
#include "bs/data/form_reader.h"
#include "bs/vm/vm.h"

typedef enum bs_event_type {
  BS_EVENT_CREATE = 0,
  BS_EVENT_DESTROY = 1,
  BS_EVENT_ALARM = 2,
  BS_EVENT_STEP = 3,
  BS_EVENT_COLLISION = 4,
  BS_EVENT_KEYBOARD = 5,
  BS_EVENT_MOUSE = 6,
  BS_EVENT_OTHER = 7,
  BS_EVENT_DRAW = 8,
  BS_EVENT_KEYPRESS = 9,
  BS_EVENT_KEYRELEASE = 10,
  BS_EVENT_TRIGGER = 11
} bs_event_type;

typedef enum bs_other_event_subtype {
  BS_OTHER_OUTSIDE_ROOM = 0,
  BS_OTHER_GAME_START = 2,
  BS_OTHER_GAME_END = 3,
  BS_OTHER_ROOM_START = 4,
  BS_OTHER_ROOM_END = 5,
  BS_OTHER_ANIMATION_END = 7
} bs_other_event_subtype;

typedef struct bs_instance {
  int32_t id;
  int32_t object_index;
  double x;
  double y;
  double xprevious;
  double yprevious;
  double xstart;
  double ystart;
  double hspeed;
  double vspeed;
  double speed;
  double direction;
  double friction;
  double gravity;
  double gravity_direction;
  int32_t mask_index;
  int32_t sprite_index;
  int32_t depth;
  bool visible;
  bool solid;
  bool persistent;
  double image_index;
  double image_speed;
  double image_xscale;
  double image_yscale;
  double image_angle;
  double image_alpha;
  double image_single;
  int32_t image_blend;
  int32_t path_index;
  double path_position;
  double path_speed;
  int32_t path_end_action;
  double path_orientation;
  double path_scale;
  double path_x_offset;
  double path_y_offset;
  int32_t alarm[12];

  int32_t *variable_indices;
  double *variable_values;
  size_t variable_count;
  size_t variable_capacity;

  bool has_been_marked_as_outside_room;
  bool destroyed;
} bs_instance;

typedef struct bs_bbox {
  double left;
  double right;
  double top;
  double bottom;
} bs_bbox;

typedef struct bs_saved_room_state {
  bs_instance *instances;
  size_t instance_count;
} bs_saved_room_state;

struct bs_game_runner;

typedef void (*bs_render_draw_sprite_callback)(void *userdata,
                                               const struct bs_game_runner *runner,
                                               int32_t sprite_index,
                                               int32_t image_index,
                                               double x,
                                               double y,
                                               int32_t blend_color,
                                               double alpha);
typedef void (*bs_render_draw_sprite_ext_callback)(void *userdata,
                                                   const struct bs_game_runner *runner,
                                                   int32_t sprite_index,
                                                   int32_t image_index,
                                                   double x,
                                                   double y,
                                                   double xscale,
                                                   double yscale,
                                                   double angle,
                                                   int32_t blend_color,
                                                   double alpha);
typedef void (*bs_render_draw_sprite_part_ext_callback)(void *userdata,
                                                        const struct bs_game_runner *runner,
                                                        int32_t sprite_index,
                                                        int32_t image_index,
                                                        int32_t left,
                                                        int32_t top,
                                                        int32_t width,
                                                        int32_t height,
                                                        double x,
                                                        double y,
                                                        double xscale,
                                                        double yscale,
                                                        int32_t blend_color,
                                                        double alpha);
typedef void (*bs_render_draw_text_callback)(void *userdata,
                                             const struct bs_game_runner *runner,
                                             const char *text,
                                             double x,
                                             double y,
                                             int32_t font_index,
                                             int32_t color,
                                             double xscale,
                                             double yscale);
typedef void (*bs_render_draw_rect_callback)(void *userdata,
                                             const struct bs_game_runner *runner,
                                             double x1,
                                             double y1,
                                             double x2,
                                             double y2,
                                             bool outline,
                                             int32_t color);
typedef void (*bs_render_clear_callback)(void *userdata, int32_t bg_color);
typedef void (*bs_render_draw_background_callback)(void *userdata,
                                                   const struct bs_game_runner *runner,
                                                   int32_t tpag_index,
                                                   int32_t x,
                                                   int32_t y,
                                                   bool tile_x,
                                                   bool tile_y);
typedef void (*bs_render_draw_tile_callback)(void *userdata,
                                             const struct bs_game_runner *runner,
                                             int32_t tpag_index,
                                             int32_t x,
                                             int32_t y,
                                             int32_t source_x,
                                             int32_t source_y,
                                             int32_t width,
                                             int32_t height,
                                             double scale_x,
                                             double scale_y,
                                             int32_t color);

typedef struct bs_render_backend {
  void *userdata;
  bs_render_clear_callback clear;
  bs_render_draw_sprite_callback draw_sprite;
  bs_render_draw_sprite_ext_callback draw_sprite_ext;
  bs_render_draw_sprite_part_ext_callback draw_sprite_part_ext;
  bs_render_draw_background_callback draw_background;
  bs_render_draw_tile_callback draw_tile;
  bs_render_draw_text_callback draw_text;
  bs_render_draw_rect_callback draw_rect;
} bs_render_backend;

/* Audio backend — implemented by the platform layer (e.g. SDL_mixer). */
typedef int32_t (*bs_audio_play_sound_callback)(void *userdata,
                                                const struct bs_game_runner *runner,
                                                int32_t sound_index,
                                                bool loop,
                                                double priority);
typedef void (*bs_audio_stop_sound_callback)(void *userdata, int32_t handle_or_index);
typedef void (*bs_audio_stop_all_callback)(void *userdata);
typedef void (*bs_audio_set_gain_callback)(void *userdata, int32_t handle, double volume, double duration_ms);
typedef void (*bs_audio_set_pitch_callback)(void *userdata, int32_t handle, double pitch);
typedef bool (*bs_audio_is_playing_callback)(void *userdata, int32_t handle_or_index);
typedef void (*bs_audio_pause_callback)(void *userdata, int32_t handle_or_index);
typedef void (*bs_audio_resume_callback)(void *userdata, int32_t handle_or_index);
typedef void (*bs_audio_set_master_gain_callback)(void *userdata, double volume);
typedef void (*bs_audio_set_track_position_callback)(void *userdata, int32_t handle, double position);
typedef double (*bs_audio_get_track_position_callback)(void *userdata, int32_t handle);

typedef struct bs_audio_backend {
  void *userdata;
  bs_audio_play_sound_callback play_sound;
  bs_audio_stop_sound_callback stop_sound;
  bs_audio_stop_all_callback stop_all;
  bs_audio_set_gain_callback set_gain;
  bs_audio_set_pitch_callback set_pitch;
  bs_audio_is_playing_callback is_playing;
  bs_audio_pause_callback pause_sound;
  bs_audio_resume_callback resume_sound;
  bs_audio_set_master_gain_callback set_master_gain;
  bs_audio_set_track_position_callback set_track_position;
  bs_audio_get_track_position_callback get_track_position;
} bs_audio_backend;

typedef struct bs_game_runner {
  const bs_game_data *game_data;
  bs_vm *vm;
  bool initialized;
  uint64_t frame_count;
  bool should_quit;

  int32_t current_room_index;
  const bs_room_data *current_room;
  int32_t pending_room_goto;
  int32_t next_instance_id;

  bs_instance *instances;
  size_t instance_count;
  size_t instance_capacity;
  bool *room_persistent_flags;
  size_t room_persistent_flag_count;
  bs_saved_room_state *saved_room_states;
  size_t saved_room_state_count;

  bool keys_held[256];
  bool keys_pressed[256];
  bool keys_released[256];
  int32_t keyboard_key;
  int32_t keyboard_lastkey;

  uint64_t total_vm_event_calls;
  uint64_t total_vm_instructions;
  bool game_started;
  bool trace_events;

  bool event_context_active;
  int32_t current_event_type;
  int32_t current_event_subtype;
  int32_t current_event_object_index;

  int32_t draw_color;
  int32_t draw_font_index;
  int32_t draw_alpha;
  int32_t draw_halign;
  int32_t draw_valign;
  int32_t image_blend;
  int32_t image_alpha;

  int32_t surface_width;
  int32_t surface_height;

  bs_render_backend render;
  bs_audio_backend audio;
} bs_game_runner;

void bs_game_runner_init(bs_game_runner *runner, const bs_game_data *game_data, bs_vm *vm);
void bs_game_runner_step(bs_game_runner *runner);
void bs_game_runner_goto_room(bs_game_runner *runner, int32_t room_index);
void bs_game_runner_on_key_down(bs_game_runner *runner, int32_t key);
void bs_game_runner_on_key_up(bs_game_runner *runner, int32_t key);
void bs_game_runner_fire_event_for_instance(bs_game_runner *runner,
                                            bs_instance *instance,
                                            int32_t event_type,
                                            int32_t subtype);
void bs_game_runner_path_end_instance(bs_game_runner *runner, bs_instance *instance);
bool bs_game_runner_compute_instance_bbox(const bs_game_runner *runner,
                                          const bs_instance *instance,
                                          bs_bbox *out_bbox);
bool bs_game_runner_instances_overlap(const bs_game_runner *runner,
                                      const bs_instance *a,
                                      const bs_instance *b);
bs_instance *bs_game_runner_find_instance_by_id(bs_game_runner *runner, int32_t id);
bool bs_game_runner_object_is_child_of(const bs_game_runner *runner, int32_t child_object_index, int32_t parent_object_index);
bs_instance *bs_game_runner_create_instance_runtime(bs_game_runner *runner,
                                                    int32_t object_index,
                                                    double x,
                                                    double y,
                                                    bool run_create_event);
void bs_game_runner_destroy_instance(bs_game_runner *runner, int32_t id);
void bs_game_runner_fire_event_inherited(bs_game_runner *runner, bs_instance *instance);
double bs_game_runner_instance_get_variable(bs_game_runner *runner,
                                            int32_t instance_id,
                                            int32_t variable_index,
                                            const char *variable_name);
bool bs_game_runner_instance_set_variable(bs_game_runner *runner,
                                          int32_t instance_id,
                                          int32_t variable_index,
                                          const char *variable_name,
                                          double value);
void bs_game_runner_dispose(bs_game_runner *runner);

#endif
