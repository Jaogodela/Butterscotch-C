#ifndef BS_DATA_FORM_READER_H
#define BS_DATA_FORM_READER_H

#include "bs/common.h"

typedef struct bs_chunk_info {
  char tag[5];
  uint32_t data_offset;
  uint32_t size;
} bs_chunk_info;

typedef struct bs_gen8_info {
  uint8_t bytecode_version;
  char *game_name;
  char *display_name;
  uint32_t game_id;
  uint32_t window_width;
  uint32_t window_height;
  uint32_t *room_order;
  size_t room_order_count;
} bs_gen8_info;

typedef struct bs_texture_page_item_data {
  uint32_t source_x;
  uint32_t source_y;
  uint32_t source_width;
  uint32_t source_height;
  uint32_t target_x;
  uint32_t target_y;
  uint32_t target_width;
  uint32_t target_height;
  uint32_t bounding_width;
  uint32_t bounding_height;
  uint32_t texture_page_id;
} bs_texture_page_item_data;

typedef struct bs_texture_page_data {
  uint32_t png_offset;
  uint32_t png_length;
} bs_texture_page_data;

typedef struct bs_sprite_data {
  char *name;
  int32_t width;
  int32_t height;
  int32_t margin_left;
  int32_t margin_right;
  int32_t margin_top;
  int32_t margin_bottom;
  int32_t origin_x;
  int32_t origin_y;
  int32_t *tpag_indices;
  size_t subimage_count;
  int32_t collision_mask_type;
} bs_sprite_data;

typedef struct bs_background_data {
  char *name;
  int32_t tpag_index;
} bs_background_data;

typedef struct bs_path_point_data {
  float x;
  float y;
  float speed;
} bs_path_point_data;

typedef struct bs_path_data {
  char *name;
  bool is_smooth;
  bool is_closed;
  int32_t precision;
  bs_path_point_data *points;
  size_t point_count;
} bs_path_data;

typedef struct bs_font_glyph_data {
  uint16_t character;
  uint16_t x;
  uint16_t y;
  uint16_t width;
  uint16_t height;
  uint16_t shift;
  uint16_t offset;
} bs_font_glyph_data;

typedef struct bs_font_data {
  char *name;
  char *display_name;
  int32_t em_size;
  int32_t tpag_index;
  float scale_x;
  float scale_y;
  bs_font_glyph_data *glyphs;
  size_t glyph_count;
} bs_font_data;

typedef enum bs_audio_format {
  BS_AUDIO_FORMAT_UNKNOWN = 0,
  BS_AUDIO_FORMAT_WAV = 1,
  BS_AUDIO_FORMAT_OGG = 2
} bs_audio_format;

typedef struct bs_audio_data {
  uint32_t data_offset;
  uint32_t length;
  bs_audio_format format;
} bs_audio_data;

typedef struct bs_sound_data {
  char *name;
  uint32_t kind;
  char *extension;
  char *file_name;
  uint32_t flags;
  float volume;
  uint32_t group_id;
  int32_t audio_id;
} bs_sound_data;

typedef struct bs_script_data {
  char *name;
  int32_t code_id;
} bs_script_data;

typedef struct bs_variable_data {
  char *name;
  int32_t instance_type;
  int32_t var_id;
  int32_t occurrence_count;
  int32_t first_occurrence_offset;
} bs_variable_data;

typedef struct bs_function_data {
  char *name;
  int32_t occurrence_count;
  int32_t first_occurrence_offset;
} bs_function_data;

typedef struct bs_event_action {
  int32_t code_id;
} bs_event_action;

typedef struct bs_event_entry {
  int32_t subtype;
  bs_event_action *actions;
  size_t action_count;
} bs_event_entry;

typedef struct bs_object_event_list {
  bs_event_entry *entries;
  size_t entry_count;
} bs_object_event_list;

typedef struct bs_game_object_data {
  char *name;
  int32_t sprite_index;
  bool visible;
  bool solid;
  int32_t depth;
  bool persistent;
  int32_t parent_id;
  int32_t mask_id;
  bs_object_event_list *events;
  size_t event_type_count;
} bs_game_object_data;

typedef struct bs_room_background_data {
  bool enabled;
  bool foreground;
  int32_t bg_def_index;
  int32_t x;
  int32_t y;
  bool tile_x;
  bool tile_y;
  int32_t speed_x;
  int32_t speed_y;
  bool stretch;
} bs_room_background_data;

typedef struct bs_room_view_data {
  bool enabled;
  int32_t view_x;
  int32_t view_y;
  int32_t view_w;
  int32_t view_h;
  int32_t port_x;
  int32_t port_y;
  int32_t port_w;
  int32_t port_h;
  int32_t border_h;
  int32_t border_v;
  int32_t speed_h;
  int32_t speed_v;
  int32_t follow_object_id;
} bs_room_view_data;

typedef struct bs_room_instance_data {
  int32_t x;
  int32_t y;
  int32_t object_def_id;
  int32_t instance_id;
  int32_t creation_code_id;
  float scale_x;
  float scale_y;
  uint32_t color;
  float rotation;
} bs_room_instance_data;

typedef struct bs_room_tile_data {
  int32_t x;
  int32_t y;
  int32_t bg_def_index;
  int32_t source_x;
  int32_t source_y;
  int32_t width;
  int32_t height;
  int32_t depth;
  int32_t instance_id;
  float scale_x;
  float scale_y;
  uint32_t color;
} bs_room_tile_data;

typedef struct bs_room_data {
  char *name;
  char *caption;
  int32_t width;
  int32_t height;
  int32_t speed;
  bool persistent;
  uint32_t bg_color;
  bool draw_bg_color;
  int32_t creation_code_id;
  uint32_t flags;
  bs_room_background_data *backgrounds;
  size_t background_count;
  bs_room_view_data *views;
  size_t view_count;
  bs_room_instance_data *instances;
  size_t instance_count;
  bs_room_tile_data *tiles;
  size_t tile_count;
} bs_room_data;

typedef struct bs_code_entry_data {
  uint32_t raw_offset;
  char *name;
  uint16_t locals_count;
  uint16_t arguments_count;
  uint32_t bytecode_absolute_offset;
  uint32_t bytecode_length;
  uint8_t *bytecode;
} bs_code_entry_data;

typedef struct bs_game_data {
  char game_path[1024];
  uint8_t *file_data;
  size_t file_size;

  uint32_t form_size;
  bs_chunk_info *chunks;
  size_t chunk_count;

  char **strings;
  size_t string_count;

  bs_gen8_info gen8;

  bs_texture_page_item_data *texture_page_items;
  uint32_t *texture_page_item_offsets;
  size_t texture_page_item_count;

  bs_texture_page_data *texture_pages;
  size_t texture_page_count;

  bs_sprite_data *sprites;
  size_t sprite_count;

  bs_background_data *backgrounds;
  size_t background_count;

  bs_path_data *paths;
  size_t path_count;

  bs_font_data *fonts;
  size_t font_count;

  bs_code_entry_data *code_entries;
  size_t code_entry_count;

  bs_sound_data *sounds;
  size_t sound_count;

  bs_audio_data *audio_data;
  size_t audio_data_count;

  bs_script_data *scripts;
  size_t script_count;

  bs_variable_data *variables;
  size_t variable_count;

  bs_function_data *functions;
  size_t function_count;

  bs_game_object_data *objects;
  size_t object_count;

  bs_room_data *rooms;
  size_t room_count;
} bs_game_data;

bool bs_form_reader_read(const char *path, bs_game_data *out_data);
void bs_game_data_free(bs_game_data *data);

#endif
