#include "bs/data/form_reader.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool can_read_range(size_t file_size, uint32_t offset, size_t need) {
  if ((size_t)offset > file_size) {
    return false;
  }
  return need <= (file_size - (size_t)offset);
}

static bool read_u8(const bs_game_data *data, uint32_t offset, uint8_t *out) {
  if (data == NULL || out == NULL || !can_read_range(data->file_size, offset, 1)) {
    return false;
  }
  *out = data->file_data[offset];
  return true;
}

static bool read_u16_le(const bs_game_data *data, uint32_t offset, uint16_t *out) {
  if (data == NULL || out == NULL || !can_read_range(data->file_size, offset, 2)) {
    return false;
  }

  const uint8_t *b = &data->file_data[offset];
  *out = (uint16_t)(((uint16_t)b[0]) | ((uint16_t)b[1] << 8));
  return true;
}

static bool read_u32_le(const bs_game_data *data, uint32_t offset, uint32_t *out) {
  if (data == NULL || out == NULL || !can_read_range(data->file_size, offset, 4)) {
    return false;
  }

  const uint8_t *b = &data->file_data[offset];
  *out = ((uint32_t)b[0]) |
         ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
  return true;
}

static bool read_i32_le(const bs_game_data *data, uint32_t offset, int32_t *out) {
  uint32_t raw = 0;
  if (!read_u32_le(data, offset, &raw)) {
    return false;
  }
  *out = (int32_t)raw;
  return true;
}

static bool read_f32_le(const bs_game_data *data, uint32_t offset, float *out) {
  uint32_t bits = 0;
  if (out == NULL || !read_u32_le(data, offset, &bits)) {
    return false;
  }
  memcpy(out, &bits, sizeof(bits));
  return true;
}

static bool read_tag(const bs_game_data *data, uint32_t offset, char out_tag[5]) {
  if (data == NULL || out_tag == NULL || !can_read_range(data->file_size, offset, 4)) {
    return false;
  }
  memcpy(out_tag, &data->file_data[offset], 4);
  out_tag[4] = '\0';
  return true;
}

static char *dup_bytes_as_string(const uint8_t *bytes, size_t len) {
  if (bytes == NULL) {
    return NULL;
  }

  char *s = (char *)malloc(len + 1);
  if (s == NULL) {
    return NULL;
  }

  if (len > 0) {
    memcpy(s, bytes, len);
  }
  s[len] = '\0';
  return s;
}

static char *read_string_at(const bs_game_data *data, uint32_t offset) {
  uint32_t len = 0;
  if (!read_u32_le(data, offset, &len)) {
    return NULL;
  }
  if (!can_read_range(data->file_size, offset + 4, (size_t)len)) {
    return NULL;
  }

  return dup_bytes_as_string(&data->file_data[offset + 4], (size_t)len);
}

static char *read_string_ref(const bs_game_data *data, uint32_t ptr) {
  if (ptr == 0) {
    return dup_bytes_as_string((const uint8_t *)"", 0);
  }
  if (ptr < 4) {
    return NULL;
  }

  uint32_t len = 0;
  if (!read_u32_le(data, ptr - 4, &len)) {
    return NULL;
  }
  if (!can_read_range(data->file_size, ptr, (size_t)len)) {
    return NULL;
  }

  return dup_bytes_as_string(&data->file_data[ptr], (size_t)len);
}

static const bs_chunk_info *find_chunk(const bs_game_data *data, const char *tag) {
  if (data == NULL || tag == NULL) {
    return NULL;
  }

  for (size_t i = 0; i < data->chunk_count; i++) {
    if (memcmp(data->chunks[i].tag, tag, 4) == 0) {
      return &data->chunks[i];
    }
  }

  return NULL;
}

static bool load_file_bytes(const char *path, uint8_t **out_bytes, size_t *out_size) {
  if (path == NULL || out_bytes == NULL || out_size == NULL) {
    return false;
  }

  *out_bytes = NULL;
  *out_size = 0;

  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    return false;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return false;
  }

  long file_len = ftell(fp);
  if (file_len < 0) {
    fclose(fp);
    return false;
  }

  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return false;
  }

  size_t file_size = (size_t)file_len;
  uint8_t *bytes = (uint8_t *)malloc(file_size);
  if (bytes == NULL) {
    fclose(fp);
    return false;
  }

  if (file_size > 0 && fread(bytes, 1, file_size, fp) != file_size) {
    free(bytes);
    fclose(fp);
    return false;
  }

  fclose(fp);

  *out_bytes = bytes;
  *out_size = file_size;
  return true;
}

static bool discover_chunks(bs_game_data *data) {
  char form_tag[5];
  if (!read_tag(data, 0, form_tag)) {
    return false;
  }
  if (memcmp(form_tag, "FORM", 4) != 0) {
    return false;
  }

  uint32_t form_size = 0;
  if (!read_u32_le(data, 4, &form_size)) {
    return false;
  }

  uint64_t form_end_u64 = 8ull + (uint64_t)form_size;
  if (form_end_u64 > (uint64_t)data->file_size) {
    return false;
  }

  size_t chunk_capacity = 32;
  size_t chunk_count = 0;
  bs_chunk_info *chunks = (bs_chunk_info *)calloc(chunk_capacity, sizeof(bs_chunk_info));
  if (chunks == NULL) {
    return false;
  }

  uint32_t offset = 8;
  const uint32_t form_end = (uint32_t)form_end_u64;
  while (offset < form_end) {
    if (!can_read_range(data->file_size, offset, 8)) {
      free(chunks);
      return false;
    }

    if (chunk_count == chunk_capacity) {
      chunk_capacity *= 2;
      bs_chunk_info *grown = (bs_chunk_info *)realloc(chunks, chunk_capacity * sizeof(bs_chunk_info));
      if (grown == NULL) {
        free(chunks);
        return false;
      }
      chunks = grown;
    }

    if (!read_tag(data, offset, chunks[chunk_count].tag)) {
      free(chunks);
      return false;
    }

    uint32_t size = 0;
    if (!read_u32_le(data, offset + 4, &size)) {
      free(chunks);
      return false;
    }

    uint64_t next_offset_u64 = (uint64_t)offset + 8ull + (uint64_t)size;
    if (next_offset_u64 > (uint64_t)form_end) {
      free(chunks);
      return false;
    }

    chunks[chunk_count].data_offset = offset + 8;
    chunks[chunk_count].size = size;
    chunk_count++;

    offset = (uint32_t)next_offset_u64;
  }

  data->form_size = form_size;
  data->chunks = chunks;
  data->chunk_count = chunk_count;
  return true;
}

static void print_chunk_list(const bs_game_data *data) {
  printf("Found %zu chunks: ", data->chunk_count);
  for (size_t i = 0; i < data->chunk_count; i++) {
    printf("%s", data->chunks[i].tag);
    if (i + 1 < data->chunk_count) {
      printf(", ");
    }
  }
  printf("\n");
}

static bool parse_strg(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "STRG");
  if (chunk == NULL) {
    return false;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }

  size_t count = (size_t)count_u32;
  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * sizeof(uint32_t))) {
    return false;
  }

  char **strings = NULL;
  if (count > 0) {
    strings = (char **)calloc(count, sizeof(char *));
    if (strings == NULL) {
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t ptr = 0;
    if (!read_u32_le(data, chunk->data_offset + 4 + (uint32_t)(i * 4), &ptr)) {
      for (size_t j = 0; j < i; j++) {
        free(strings[j]);
      }
      free(strings);
      return false;
    }

    strings[i] = read_string_at(data, ptr);
    if (strings[i] == NULL) {
      for (size_t j = 0; j < i; j++) {
        free(strings[j]);
      }
      free(strings);
      return false;
    }
  }

  data->strings = strings;
  data->string_count = count;
  printf("  STRG: %u strings\n", count_u32);
  return true;
}

static bool parse_gen8(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "GEN8");
  if (chunk == NULL) {
    return false;
  }

  const uint32_t d = chunk->data_offset;
  uint8_t bytecode_version = 0;
  uint32_t game_name_ptr = 0;
  uint32_t display_name_ptr = 0;
  uint32_t game_id = 0;
  uint32_t window_width = 0;
  uint32_t window_height = 0;
  uint32_t room_order_count_u32 = 0;

  if (!read_u8(data, d + 1, &bytecode_version) ||
      !read_u32_le(data, d + 0x28, &game_name_ptr) ||
      !read_u32_le(data, d + 0x64, &display_name_ptr) ||
      !read_u32_le(data, d + 0x14, &game_id) ||
      !read_u32_le(data, d + 0x3C, &window_width) ||
      !read_u32_le(data, d + 0x40, &window_height) ||
      !read_u32_le(data, d + 0x80, &room_order_count_u32)) {
    return false;
  }

  size_t room_order_count = (size_t)room_order_count_u32;
  if (!can_read_range(data->file_size, d + 0x84, room_order_count * sizeof(uint32_t))) {
    return false;
  }

  uint32_t *room_order = NULL;
  if (room_order_count > 0) {
    room_order = (uint32_t *)calloc(room_order_count, sizeof(uint32_t));
    if (room_order == NULL) {
      return false;
    }
  }

  for (size_t i = 0; i < room_order_count; i++) {
    if (!read_u32_le(data, d + 0x84 + (uint32_t)(i * 4), &room_order[i])) {
      free(room_order);
      return false;
    }
  }

  char *game_name = read_string_ref(data, game_name_ptr);
  char *display_name = read_string_ref(data, display_name_ptr);
  if (game_name == NULL || display_name == NULL) {
    free(game_name);
    free(display_name);
    free(room_order);
    return false;
  }

  data->gen8.bytecode_version = bytecode_version;
  data->gen8.game_name = game_name;
  data->gen8.display_name = display_name;
  data->gen8.game_id = game_id;
  data->gen8.window_width = window_width;
  data->gen8.window_height = window_height;
  data->gen8.room_order = room_order;
  data->gen8.room_order_count = room_order_count;

  printf("  GEN8: '%s' BC%u %ux%u %zu rooms\n",
         data->gen8.game_name,
         (unsigned)data->gen8.bytecode_version,
         data->gen8.window_width,
         data->gen8.window_height,
         data->gen8.room_order_count);
  return true;
}

static bool parse_tpag(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "TPAG");
  if (chunk == NULL) {
    return false;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }
  size_t count = (size_t)count_u32;

  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * sizeof(uint32_t))) {
    return false;
  }

  bs_texture_page_item_data *items = NULL;
  uint32_t *offsets = NULL;
  if (count > 0) {
    items = (bs_texture_page_item_data *)calloc(count, sizeof(bs_texture_page_item_data));
    offsets = (uint32_t *)calloc(count, sizeof(uint32_t));
    if (items == NULL || offsets == NULL) {
      free(items);
      free(offsets);
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t ptr = 0;
    if (!read_u32_le(data, chunk->data_offset + 4 + (uint32_t)(i * 4), &ptr)) {
      free(items);
      free(offsets);
      return false;
    }
    offsets[i] = ptr;

    uint16_t v16 = 0;
    if (!read_u16_le(data, ptr + 0, &v16)) { free(items); free(offsets); return false; }
    items[i].source_x = (uint32_t)v16;
    if (!read_u16_le(data, ptr + 2, &v16)) { free(items); free(offsets); return false; }
    items[i].source_y = (uint32_t)v16;
    if (!read_u16_le(data, ptr + 4, &v16)) { free(items); free(offsets); return false; }
    items[i].source_width = (uint32_t)v16;
    if (!read_u16_le(data, ptr + 6, &v16)) { free(items); free(offsets); return false; }
    items[i].source_height = (uint32_t)v16;
    if (!read_u16_le(data, ptr + 8, &v16)) { free(items); free(offsets); return false; }
    items[i].target_x = (uint32_t)v16;
    if (!read_u16_le(data, ptr + 10, &v16)) { free(items); free(offsets); return false; }
    items[i].target_y = (uint32_t)v16;
    if (!read_u16_le(data, ptr + 12, &v16)) { free(items); free(offsets); return false; }
    items[i].target_width = (uint32_t)v16;
    if (!read_u16_le(data, ptr + 14, &v16)) { free(items); free(offsets); return false; }
    items[i].target_height = (uint32_t)v16;
    if (!read_u16_le(data, ptr + 16, &v16)) { free(items); free(offsets); return false; }
    items[i].bounding_width = (uint32_t)v16;
    if (!read_u16_le(data, ptr + 18, &v16)) { free(items); free(offsets); return false; }
    items[i].bounding_height = (uint32_t)v16;
    if (!read_u16_le(data, ptr + 20, &v16)) { free(items); free(offsets); return false; }
    items[i].texture_page_id = (uint32_t)v16;
  }

  data->texture_page_items = items;
  data->texture_page_item_offsets = offsets;
  data->texture_page_item_count = count;
  printf("  TPAG: %u items\n", count_u32);
  return true;
}

static bool parse_txtr(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "TXTR");
  if (chunk == NULL) {
    return false;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }

  size_t count = (size_t)count_u32;
  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * sizeof(uint32_t))) {
    return false;
  }

  bs_texture_page_data *pages = NULL;
  uint32_t *offsets = NULL;
  if (count > 0) {
    pages = (bs_texture_page_data *)calloc(count, sizeof(bs_texture_page_data));
    offsets = (uint32_t *)calloc(count, sizeof(uint32_t));
    if (pages == NULL || offsets == NULL) {
      free(pages);
      free(offsets);
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t ptr = 0;
    if (!read_u32_le(data, chunk->data_offset + 4 + (uint32_t)(i * 4), &ptr)) {
      free(pages);
      free(offsets);
      return false;
    }
    if (!read_u32_le(data, ptr + 4, &offsets[i])) {
      free(pages);
      free(offsets);
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t png_offset = offsets[i];
    uint32_t png_end = (i + 1 < count) ? offsets[i + 1] : (chunk->data_offset + chunk->size);
    if (png_end < png_offset) {
      free(pages);
      free(offsets);
      return false;
    }
    pages[i].png_offset = png_offset;
    pages[i].png_length = png_end - png_offset;
  }

  free(offsets);
  data->texture_pages = pages;
  data->texture_page_count = count;
  printf("  TXTR: %u texture pages\n", count_u32);
  return true;
}

static int32_t resolve_tpag_index_by_offset(const bs_game_data *data, uint32_t raw_offset) {
  if (data == NULL) {
    return -1;
  }
  for (size_t i = 0; i < data->texture_page_item_count; i++) {
    if (data->texture_page_item_offsets != NULL && data->texture_page_item_offsets[i] == raw_offset) {
      return (int32_t)i;
    }
  }
  return -1;
}

static bool parse_sprt(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "SPRT");
  if (chunk == NULL) {
    data->sprites = NULL;
    data->sprite_count = 0;
    return true;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }
  size_t count = (size_t)count_u32;
  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * sizeof(uint32_t))) {
    return false;
  }

  bs_sprite_data *sprites = NULL;
  if (count > 0) {
    sprites = (bs_sprite_data *)calloc(count, sizeof(bs_sprite_data));
    if (sprites == NULL) {
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t ptr = 0;
    uint32_t name_ptr = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t margin_left = 0;
    int32_t margin_right = 0;
    int32_t margin_bottom = 0;
    int32_t margin_top = 0;
    int32_t collision_mask_type = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    int32_t subimage_count_i32 = 0;
    size_t subimage_count = 0;
    int32_t *tpag_indices = NULL;
    char *name = NULL;

    if (!read_u32_le(data, chunk->data_offset + 4 + (uint32_t)(i * 4), &ptr) ||
        !read_u32_le(data, ptr + 0, &name_ptr) ||
        !read_i32_le(data, ptr + 4, &width) ||
        !read_i32_le(data, ptr + 8, &height) ||
        !read_i32_le(data, ptr + 0x0C, &margin_left) ||
        !read_i32_le(data, ptr + 0x10, &margin_right) ||
        !read_i32_le(data, ptr + 0x14, &margin_bottom) ||
        !read_i32_le(data, ptr + 0x18, &margin_top) ||
        !read_i32_le(data, ptr + 0x2C, &collision_mask_type) ||
        !read_i32_le(data, ptr + 0x30, &origin_x) ||
        !read_i32_le(data, ptr + 0x34, &origin_y) ||
        !read_i32_le(data, ptr + 0x38, &subimage_count_i32)) {
      for (size_t j = 0; j < i; j++) {
        free(sprites[j].name);
        free(sprites[j].tpag_indices);
      }
      free(sprites);
      return false;
    }

    if (subimage_count_i32 < 0) {
      for (size_t j = 0; j < i; j++) {
        free(sprites[j].name);
        free(sprites[j].tpag_indices);
      }
      free(sprites);
      return false;
    }
    subimage_count = (size_t)subimage_count_i32;

    if (!can_read_range(data->file_size, ptr + 0x3C, subimage_count * sizeof(uint32_t))) {
      for (size_t j = 0; j < i; j++) {
        free(sprites[j].name);
        free(sprites[j].tpag_indices);
      }
      free(sprites);
      return false;
    }

    if (subimage_count > 0) {
      tpag_indices = (int32_t *)calloc(subimage_count, sizeof(int32_t));
      if (tpag_indices == NULL) {
        for (size_t j = 0; j < i; j++) {
          free(sprites[j].name);
          free(sprites[j].tpag_indices);
        }
        free(sprites);
        return false;
      }
    }

    for (size_t frame = 0; frame < subimage_count; frame++) {
      uint32_t tpag_ptr = 0;
      if (!read_u32_le(data, ptr + 0x3C + (uint32_t)(frame * 4), &tpag_ptr)) {
        free(tpag_indices);
        for (size_t j = 0; j < i; j++) {
          free(sprites[j].name);
          free(sprites[j].tpag_indices);
        }
        free(sprites);
        return false;
      }
      tpag_indices[frame] = resolve_tpag_index_by_offset(data, tpag_ptr);
    }

    name = read_string_ref(data, name_ptr);
    if (name == NULL) {
      free(tpag_indices);
      for (size_t j = 0; j < i; j++) {
        free(sprites[j].name);
        free(sprites[j].tpag_indices);
      }
      free(sprites);
      return false;
    }

    sprites[i].name = name;
    sprites[i].width = width;
    sprites[i].height = height;
    sprites[i].margin_left = margin_left;
    sprites[i].margin_right = margin_right;
    sprites[i].margin_top = margin_top;
    sprites[i].margin_bottom = margin_bottom;
    sprites[i].origin_x = origin_x;
    sprites[i].origin_y = origin_y;
    sprites[i].tpag_indices = tpag_indices;
    sprites[i].subimage_count = subimage_count;
    sprites[i].collision_mask_type = collision_mask_type;
  }

  data->sprites = sprites;
  data->sprite_count = count;
  printf("  SPRT: %u sprites\n", count_u32);
  return true;
}

static bool parse_bgnd(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "BGND");
  if (chunk == NULL) {
    data->backgrounds = NULL;
    data->background_count = 0;
    return true;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }
  size_t count = (size_t)count_u32;
  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * sizeof(uint32_t))) {
    return false;
  }

  bs_background_data *backgrounds = NULL;
  if (count > 0) {
    backgrounds = (bs_background_data *)calloc(count, sizeof(bs_background_data));
    if (backgrounds == NULL) {
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t ptr = 0;
    uint32_t name_ptr = 0;
    uint32_t tpag_ptr = 0;
    if (!read_u32_le(data, chunk->data_offset + 4 + (uint32_t)(i * 4), &ptr) ||
        !read_u32_le(data, ptr + 0, &name_ptr) ||
        !read_u32_le(data, ptr + 0x10, &tpag_ptr)) {
      for (size_t j = 0; j < i; j++) {
        free(backgrounds[j].name);
      }
      free(backgrounds);
      return false;
    }

    backgrounds[i].name = read_string_ref(data, name_ptr);
    if (backgrounds[i].name == NULL) {
      for (size_t j = 0; j <= i; j++) {
        free(backgrounds[j].name);
      }
      free(backgrounds);
      return false;
    }
    backgrounds[i].tpag_index = resolve_tpag_index_by_offset(data, tpag_ptr);
  }

  data->backgrounds = backgrounds;
  data->background_count = count;
  printf("  BGND: %u backgrounds\n", count_u32);
  return true;
}

static bool parse_path(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "PATH");
  if (chunk == NULL) {
    data->paths = NULL;
    data->path_count = 0;
    return true;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }
  size_t count = (size_t)count_u32;
  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * sizeof(uint32_t))) {
    return false;
  }

  bs_path_data *paths = NULL;
  if (count > 0) {
    paths = (bs_path_data *)calloc(count, sizeof(bs_path_data));
    if (paths == NULL) {
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t ptr = 0;
    uint32_t name_ptr = 0;
    uint32_t is_smooth_u32 = 0;
    uint32_t is_closed_u32 = 0;
    int32_t precision = 0;
    uint32_t point_count_u32 = 0;
    bs_path_point_data *points = NULL;
    char *name = NULL;

    if (!read_u32_le(data, chunk->data_offset + 4 + (uint32_t)(i * 4), &ptr) ||
        !read_u32_le(data, ptr + 0, &name_ptr) ||
        !read_u32_le(data, ptr + 4, &is_smooth_u32) ||
        !read_u32_le(data, ptr + 8, &is_closed_u32) ||
        !read_i32_le(data, ptr + 12, &precision) ||
        !read_u32_le(data, ptr + 16, &point_count_u32)) {
      for (size_t j = 0; j < i; j++) {
        free(paths[j].name);
        free(paths[j].points);
      }
      free(paths);
      return false;
    }

    if (!can_read_range(data->file_size, ptr + 20, (size_t)point_count_u32 * 12u)) {
      for (size_t j = 0; j < i; j++) {
        free(paths[j].name);
        free(paths[j].points);
      }
      free(paths);
      return false;
    }

    if (point_count_u32 > 0) {
      points = (bs_path_point_data *)calloc((size_t)point_count_u32, sizeof(bs_path_point_data));
      if (points == NULL) {
        for (size_t j = 0; j < i; j++) {
          free(paths[j].name);
          free(paths[j].points);
        }
        free(paths);
        return false;
      }
    }

    for (size_t p = 0; p < (size_t)point_count_u32; p++) {
      uint32_t point_offset = ptr + 20 + (uint32_t)(p * 12u);
      if (!read_f32_le(data, point_offset + 0, &points[p].x) ||
          !read_f32_le(data, point_offset + 4, &points[p].y) ||
          !read_f32_le(data, point_offset + 8, &points[p].speed)) {
        free(points);
        for (size_t j = 0; j < i; j++) {
          free(paths[j].name);
          free(paths[j].points);
        }
        free(paths);
        return false;
      }
    }

    name = read_string_ref(data, name_ptr);
    if (name == NULL) {
      free(points);
      for (size_t j = 0; j < i; j++) {
        free(paths[j].name);
        free(paths[j].points);
      }
      free(paths);
      return false;
    }

    paths[i].name = name;
    paths[i].is_smooth = (is_smooth_u32 != 0);
    paths[i].is_closed = (is_closed_u32 != 0);
    paths[i].precision = precision;
    paths[i].points = points;
    paths[i].point_count = (size_t)point_count_u32;
  }

  data->paths = paths;
  data->path_count = count;
  printf("  PATH: %u paths\n", count_u32);
  return true;
}

static bool parse_font(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "FONT");
  if (chunk == NULL) {
    data->fonts = NULL;
    data->font_count = 0;
    return true;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }
  size_t count = (size_t)count_u32;
  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * sizeof(uint32_t))) {
    return false;
  }

  bs_font_data *fonts = NULL;
  if (count > 0) {
    fonts = (bs_font_data *)calloc(count, sizeof(bs_font_data));
    if (fonts == NULL) {
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t ptr = 0;
    uint32_t name_ptr = 0;
    uint32_t display_name_ptr = 0;
    int32_t em_size = 0;
    uint32_t tpag_ptr = 0;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    uint32_t glyph_count_u32 = 0;
    bs_font_glyph_data *glyphs = NULL;
    char *name = NULL;
    char *display_name = NULL;

    if (!read_u32_le(data, chunk->data_offset + 4 + (uint32_t)(i * 4), &ptr) ||
        !read_u32_le(data, ptr + 0, &name_ptr) ||
        !read_u32_le(data, ptr + 4, &display_name_ptr) ||
        !read_i32_le(data, ptr + 8, &em_size) ||
        !read_u32_le(data, ptr + 28, &tpag_ptr) ||
        !read_f32_le(data, ptr + 32, &scale_x) ||
        !read_f32_le(data, ptr + 36, &scale_y) ||
        !read_u32_le(data, ptr + 40, &glyph_count_u32)) {
      for (size_t j = 0; j < i; j++) {
        free(fonts[j].name);
        free(fonts[j].display_name);
        free(fonts[j].glyphs);
      }
      free(fonts);
      return false;
    }

    if (!can_read_range(data->file_size, ptr + 44, (size_t)glyph_count_u32 * sizeof(uint32_t))) {
      for (size_t j = 0; j < i; j++) {
        free(fonts[j].name);
        free(fonts[j].display_name);
        free(fonts[j].glyphs);
      }
      free(fonts);
      return false;
    }

    if (glyph_count_u32 > 0) {
      glyphs = (bs_font_glyph_data *)calloc((size_t)glyph_count_u32, sizeof(bs_font_glyph_data));
      if (glyphs == NULL) {
        for (size_t j = 0; j < i; j++) {
          free(fonts[j].name);
          free(fonts[j].display_name);
          free(fonts[j].glyphs);
        }
        free(fonts);
        return false;
      }
    }

    for (size_t g = 0; g < (size_t)glyph_count_u32; g++) {
      uint32_t glyph_ptr = 0;
      if (!read_u32_le(data, ptr + 44 + (uint32_t)(g * 4), &glyph_ptr) ||
          !read_u16_le(data, glyph_ptr + 0, &glyphs[g].character) ||
          !read_u16_le(data, glyph_ptr + 2, &glyphs[g].x) ||
          !read_u16_le(data, glyph_ptr + 4, &glyphs[g].y) ||
          !read_u16_le(data, glyph_ptr + 6, &glyphs[g].width) ||
          !read_u16_le(data, glyph_ptr + 8, &glyphs[g].height) ||
          !read_u16_le(data, glyph_ptr + 10, &glyphs[g].shift) ||
          !read_u16_le(data, glyph_ptr + 12, &glyphs[g].offset)) {
        free(glyphs);
        for (size_t j = 0; j < i; j++) {
          free(fonts[j].name);
          free(fonts[j].display_name);
          free(fonts[j].glyphs);
        }
        free(fonts);
        return false;
      }
    }

    name = read_string_ref(data, name_ptr);
    display_name = read_string_ref(data, display_name_ptr);
    if (name == NULL || display_name == NULL) {
      free(name);
      free(display_name);
      free(glyphs);
      for (size_t j = 0; j < i; j++) {
        free(fonts[j].name);
        free(fonts[j].display_name);
        free(fonts[j].glyphs);
      }
      free(fonts);
      return false;
    }

    fonts[i].name = name;
    fonts[i].display_name = display_name;
    fonts[i].em_size = em_size;
    fonts[i].tpag_index = resolve_tpag_index_by_offset(data, tpag_ptr);
    fonts[i].scale_x = scale_x;
    fonts[i].scale_y = scale_y;
    fonts[i].glyphs = glyphs;
    fonts[i].glyph_count = (size_t)glyph_count_u32;
  }

  data->fonts = fonts;
  data->font_count = count;
  printf("  FONT: %u fonts\n", count_u32);
  return true;
}

static bool parse_code(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "CODE");
  if (chunk == NULL) {
    return false;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }

  size_t count = (size_t)count_u32;
  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * sizeof(uint32_t))) {
    return false;
  }

  bs_code_entry_data *entries = NULL;
  if (count > 0) {
    entries = (bs_code_entry_data *)calloc(count, sizeof(bs_code_entry_data));
    if (entries == NULL) {
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t ptr = 0;
    if (!read_u32_le(data, chunk->data_offset + 4 + (uint32_t)(i * 4), &ptr)) {
      free(entries);
      return false;
    }

    uint32_t name_ptr = 0;
    uint32_t length = 0;
    uint16_t locals_count = 0;
    uint16_t args_count_raw = 0;
    int32_t relative_offset = 0;
    if (!read_u32_le(data, ptr + 0, &name_ptr) ||
        !read_u32_le(data, ptr + 4, &length) ||
        !read_u16_le(data, ptr + 8, &locals_count) ||
        !read_u16_le(data, ptr + 10, &args_count_raw) ||
        !read_i32_le(data, ptr + 12, &relative_offset)) {
      for (size_t j = 0; j < i; j++) {
        free(entries[j].name);
        free(entries[j].bytecode);
      }
      free(entries);
      return false;
    }

    int64_t bytecode_addr_i64 = (int64_t)ptr + 12ll + (int64_t)relative_offset;
    if (bytecode_addr_i64 < 0 || (uint64_t)bytecode_addr_i64 > (uint64_t)data->file_size) {
      for (size_t j = 0; j < i; j++) {
        free(entries[j].name);
        free(entries[j].bytecode);
      }
      free(entries);
      return false;
    }

    uint32_t bytecode_addr = (uint32_t)bytecode_addr_i64;
    if (!can_read_range(data->file_size, bytecode_addr, (size_t)length)) {
      for (size_t j = 0; j < i; j++) {
        free(entries[j].name);
        free(entries[j].bytecode);
      }
      free(entries);
      return false;
    }

    char *name = read_string_ref(data, name_ptr);
    if (name == NULL) {
      for (size_t j = 0; j < i; j++) {
        free(entries[j].name);
        free(entries[j].bytecode);
      }
      free(entries);
      return false;
    }

    uint8_t *bytecode = NULL;
    if (length > 0) {
      bytecode = (uint8_t *)malloc((size_t)length);
      if (bytecode == NULL) {
        free(name);
        for (size_t j = 0; j < i; j++) {
          free(entries[j].name);
          free(entries[j].bytecode);
        }
        free(entries);
        return false;
      }
      memcpy(bytecode, &data->file_data[bytecode_addr], (size_t)length);
    }

    entries[i].raw_offset = ptr;
    entries[i].name = name;
    entries[i].locals_count = locals_count;
    entries[i].arguments_count = (uint16_t)(args_count_raw & 0x7FFFu);
    entries[i].bytecode_absolute_offset = bytecode_addr;
    entries[i].bytecode_length = length;
    entries[i].bytecode = bytecode;
  }

  data->code_entries = entries;
  data->code_entry_count = count;
  printf("  CODE: %u entries\n", count_u32);
  return true;
}

static int32_t resolve_code_index(const bs_game_data *data, int32_t raw_code_id) {
  if (data == NULL) {
    return -1;
  }
  if (raw_code_id < 0) {
    return raw_code_id;
  }

  uint32_t raw_u32 = (uint32_t)raw_code_id;
  for (size_t i = 0; i < data->code_entry_count; i++) {
    if (data->code_entries[i].raw_offset == raw_u32) {
      return (int32_t)i;
    }
  }

  return raw_code_id;
}

static bool parse_sond(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "SOND");
  if (chunk == NULL) {
    data->sounds = NULL;
    data->sound_count = 0;
    return true;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }
  size_t count = (size_t)count_u32;

  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * sizeof(uint32_t))) {
    return false;
  }

  bs_sound_data *sounds = NULL;
  if (count > 0) {
    sounds = (bs_sound_data *)calloc(count, sizeof(bs_sound_data));
    if (sounds == NULL) {
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t ptr = 0;
    uint32_t name_ptr = 0;
    uint32_t kind = 0;
    uint32_t ext_ptr = 0;
    uint32_t file_ptr = 0;
    uint32_t flags = 0;
    float volume = 0.0f;
    uint32_t group_id = 0;
    int32_t audio_id = -1;

    if (!read_u32_le(data, chunk->data_offset + 4 + (uint32_t)(i * 4), &ptr) ||
        !read_u32_le(data, ptr + 0, &name_ptr) ||
        !read_u32_le(data, ptr + 4, &kind) ||
        !read_u32_le(data, ptr + 8, &ext_ptr) ||
        !read_u32_le(data, ptr + 12, &file_ptr) ||
        !read_u32_le(data, ptr + 16, &flags) ||
        !read_f32_le(data, ptr + 20, &volume) ||
        !read_u32_le(data, ptr + 24, &group_id) ||
        !read_i32_le(data, ptr + 32, &audio_id)) {
      for (size_t j = 0; j < i; j++) {
        free(sounds[j].name);
        free(sounds[j].extension);
        free(sounds[j].file_name);
      }
      free(sounds);
      return false;
    }

    sounds[i].name = read_string_ref(data, name_ptr);
    sounds[i].extension = read_string_ref(data, ext_ptr);
    sounds[i].file_name = read_string_ref(data, file_ptr);
    if (sounds[i].name == NULL || sounds[i].extension == NULL || sounds[i].file_name == NULL) {
      free(sounds[i].name);
      free(sounds[i].extension);
      free(sounds[i].file_name);
      for (size_t j = 0; j < i; j++) {
        free(sounds[j].name);
        free(sounds[j].extension);
        free(sounds[j].file_name);
      }
      free(sounds);
      return false;
    }

    sounds[i].kind = kind;
    sounds[i].flags = flags;
    sounds[i].volume = volume;
    sounds[i].group_id = group_id;
    sounds[i].audio_id = audio_id;
  }

  data->sounds = sounds;
  data->sound_count = count;
  printf("  SOND: %u sounds\n", count_u32);
  return true;
}

static bool parse_audo(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "AUDO");
  if (chunk == NULL) {
    data->audio_data = NULL;
    data->audio_data_count = 0;
    return true;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }
  size_t count = (size_t)count_u32;

  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * sizeof(uint32_t))) {
    return false;
  }

  bs_audio_data *audio_data = NULL;
  if (count > 0) {
    audio_data = (bs_audio_data *)calloc(count, sizeof(bs_audio_data));
    if (audio_data == NULL) {
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t ptr = 0;
    uint32_t length = 0;
    if (!read_u32_le(data, chunk->data_offset + 4 + (uint32_t)(i * 4), &ptr) ||
        !read_u32_le(data, ptr, &length)) {
      free(audio_data);
      return false;
    }

    uint32_t data_offset = ptr + 4;
    char tag[5];
    if (!read_tag(data, data_offset, tag)) {
      free(audio_data);
      return false;
    }

    bs_audio_format format = BS_AUDIO_FORMAT_UNKNOWN;
    if (memcmp(tag, "RIFF", 4) == 0) {
      format = BS_AUDIO_FORMAT_WAV;
    } else if (memcmp(tag, "OggS", 4) == 0) {
      format = BS_AUDIO_FORMAT_OGG;
    }

    audio_data[i].data_offset = data_offset;
    audio_data[i].length = length;
    audio_data[i].format = format;
  }

  data->audio_data = audio_data;
  data->audio_data_count = count;
  printf("  AUDO: %u audio entries\n", count_u32);
  return true;
}

static bool parse_scpt(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "SCPT");
  if (chunk == NULL) {
    return false;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }
  size_t count = (size_t)count_u32;

  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * sizeof(uint32_t))) {
    return false;
  }

  bs_script_data *scripts = NULL;
  if (count > 0) {
    scripts = (bs_script_data *)calloc(count, sizeof(bs_script_data));
    if (scripts == NULL) {
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t ptr = 0;
    uint32_t name_ptr = 0;
    int32_t code_id = -1;
    if (!read_u32_le(data, chunk->data_offset + 4 + (uint32_t)(i * 4), &ptr) ||
        !read_u32_le(data, ptr, &name_ptr) ||
        !read_i32_le(data, ptr + 4, &code_id)) {
      for (size_t j = 0; j < i; j++) {
        free(scripts[j].name);
      }
      free(scripts);
      return false;
    }

    scripts[i].name = read_string_ref(data, name_ptr);
    if (scripts[i].name == NULL) {
      for (size_t j = 0; j < i; j++) {
        free(scripts[j].name);
      }
      free(scripts);
      return false;
    }
    scripts[i].code_id = code_id;
  }

  data->scripts = scripts;
  data->script_count = count;
  printf("  SCPT: %u scripts\n", count_u32);
  return true;
}

static bool parse_vari(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "VARI");
  if (chunk == NULL) {
    return false;
  }

  const uint32_t d = chunk->data_offset;
  const uint32_t end = chunk->data_offset + chunk->size;
  if (!can_read_range(data->file_size, d, 12)) {
    return false;
  }

  uint32_t count1 = 0;
  uint32_t count2 = 0;
  uint32_t max_local = 0;
  if (!read_u32_le(data, d, &count1) ||
      !read_u32_le(data, d + 4, &count2) ||
      !read_u32_le(data, d + 8, &max_local)) {
    return false;
  }

  size_t capacity = 1024;
  size_t count = 0;
  bs_variable_data *variables = (bs_variable_data *)calloc(capacity, sizeof(bs_variable_data));
  if (variables == NULL) {
    return false;
  }

  uint32_t offset = d + 12;
  while (offset + 20 <= end) {
    if (count == capacity) {
      capacity *= 2;
      bs_variable_data *grown = (bs_variable_data *)realloc(variables, capacity * sizeof(bs_variable_data));
      if (grown == NULL) {
        for (size_t i = 0; i < count; i++) {
          free(variables[i].name);
        }
        free(variables);
        return false;
      }
      variables = grown;
    }

    uint32_t name_ptr = 0;
    int32_t instance_type = 0;
    int32_t var_id = 0;
    int32_t occ_count = 0;
    int32_t first_occ = 0;
    if (!read_u32_le(data, offset, &name_ptr) ||
        !read_i32_le(data, offset + 4, &instance_type) ||
        !read_i32_le(data, offset + 8, &var_id) ||
        !read_i32_le(data, offset + 12, &occ_count) ||
        !read_i32_le(data, offset + 16, &first_occ)) {
      for (size_t i = 0; i < count; i++) {
        free(variables[i].name);
      }
      free(variables);
      return false;
    }

    variables[count].name = read_string_ref(data, name_ptr);
    if (variables[count].name == NULL) {
      for (size_t i = 0; i < count; i++) {
        free(variables[i].name);
      }
      free(variables);
      return false;
    }
    variables[count].instance_type = instance_type;
    variables[count].var_id = var_id;
    variables[count].occurrence_count = occ_count;
    variables[count].first_occurrence_offset = first_occ;
    count++;

    offset += 20;
  }

  data->variables = variables;
  data->variable_count = count;
  (void)count2;
  printf("  VARI: %zu variables (count1=%u, maxLocal=%u)\n", count, count1, max_local);
  return true;
}

static bool parse_func(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "FUNC");
  if (chunk == NULL) {
    return false;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }
  size_t count = (size_t)count_u32;

  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * 12u)) {
    return false;
  }

  bs_function_data *functions = NULL;
  if (count > 0) {
    functions = (bs_function_data *)calloc(count, sizeof(bs_function_data));
    if (functions == NULL) {
      return false;
    }
  }

  uint32_t offset = chunk->data_offset + 4;
  for (size_t i = 0; i < count; i++) {
    uint32_t name_ptr = 0;
    int32_t occ_count = 0;
    int32_t first_occ = 0;
    if (!read_u32_le(data, offset, &name_ptr) ||
        !read_i32_le(data, offset + 4, &occ_count) ||
        !read_i32_le(data, offset + 8, &first_occ)) {
      for (size_t j = 0; j < i; j++) {
        free(functions[j].name);
      }
      free(functions);
      return false;
    }

    functions[i].name = read_string_ref(data, name_ptr);
    if (functions[i].name == NULL) {
      for (size_t j = 0; j < i; j++) {
        free(functions[j].name);
      }
      free(functions);
      return false;
    }
    functions[i].occurrence_count = occ_count;
    functions[i].first_occurrence_offset = first_occ;
    offset += 12;
  }

  data->functions = functions;
  data->function_count = count;
  printf("  FUNC: %u functions\n", count_u32);
  return true;
}

static void free_sprite_data(bs_sprite_data *sprite) {
  if (sprite == NULL) {
    return;
  }
  free(sprite->name);
  free(sprite->tpag_indices);
  sprite->name = NULL;
  sprite->tpag_indices = NULL;
  sprite->subimage_count = 0;
}

static void free_background_data(bs_background_data *background) {
  if (background == NULL) {
    return;
  }
  free(background->name);
  background->name = NULL;
  background->tpag_index = -1;
}

static void free_path_data(bs_path_data *path) {
  if (path == NULL) {
    return;
  }
  free(path->name);
  free(path->points);
  path->name = NULL;
  path->points = NULL;
  path->point_count = 0;
}

static void free_font_data(bs_font_data *font) {
  if (font == NULL) {
    return;
  }
  free(font->name);
  free(font->display_name);
  free(font->glyphs);
  font->name = NULL;
  font->display_name = NULL;
  font->glyphs = NULL;
  font->glyph_count = 0;
}

static void free_object_data(bs_game_object_data *obj) {
  if (obj == NULL) {
    return;
  }

  free(obj->name);
  obj->name = NULL;

  for (size_t event_type = 0; event_type < obj->event_type_count; event_type++) {
    bs_object_event_list *event_list = &obj->events[event_type];
    for (size_t i = 0; i < event_list->entry_count; i++) {
      free(event_list->entries[i].actions);
      event_list->entries[i].actions = NULL;
      event_list->entries[i].action_count = 0;
    }
    free(event_list->entries);
    event_list->entries = NULL;
    event_list->entry_count = 0;
  }

  free(obj->events);
  obj->events = NULL;
  obj->event_type_count = 0;
}

static void free_room_data(bs_room_data *room) {
  if (room == NULL) {
    return;
  }

  free(room->name);
  free(room->caption);
  room->name = NULL;
  room->caption = NULL;

  free(room->backgrounds);
  room->backgrounds = NULL;
  room->background_count = 0;

  free(room->views);
  room->views = NULL;
  room->view_count = 0;

  free(room->instances);
  room->instances = NULL;
  room->instance_count = 0;

  free(room->tiles);
  room->tiles = NULL;
  room->tile_count = 0;
}

static bool parse_objt(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "OBJT");
  if (chunk == NULL) {
    return false;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }
  size_t count = (size_t)count_u32;

  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * sizeof(uint32_t))) {
    return false;
  }

  bs_game_object_data *objects = NULL;
  if (count > 0) {
    objects = (bs_game_object_data *)calloc(count, sizeof(bs_game_object_data));
    if (objects == NULL) {
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t ptr = 0;
    uint32_t name_ptr = 0;
    int32_t sprite_index = -1;
    uint32_t visible_u32 = 0;
    uint32_t solid_u32 = 0;
    int32_t depth = 0;
    uint32_t persistent_u32 = 0;
    int32_t parent_id = -1;
    int32_t mask_id = -1;
    int32_t physics_vertex_count = 0;

    if (!read_u32_le(data, chunk->data_offset + 4 + (uint32_t)(i * 4), &ptr) ||
        !read_u32_le(data, ptr + 0, &name_ptr) ||
        !read_i32_le(data, ptr + 4, &sprite_index) ||
        !read_u32_le(data, ptr + 8, &visible_u32) ||
        !read_u32_le(data, ptr + 0x0C, &solid_u32) ||
        !read_i32_le(data, ptr + 0x10, &depth) ||
        !read_u32_le(data, ptr + 0x14, &persistent_u32) ||
        !read_i32_le(data, ptr + 0x18, &parent_id) ||
        !read_i32_le(data, ptr + 0x1C, &mask_id) ||
        !read_i32_le(data, ptr + 0x40, &physics_vertex_count)) {
      for (size_t j = 0; j < i; j++) {
        free_object_data(&objects[j]);
      }
      free(objects);
      return false;
    }

    if (physics_vertex_count < 0) {
      for (size_t j = 0; j < i; j++) {
        free_object_data(&objects[j]);
      }
      free(objects);
      return false;
    }

    uint64_t events_start_u64 = (uint64_t)ptr + 0x50ull + ((uint64_t)physics_vertex_count * 8ull);
    if (events_start_u64 > (uint64_t)data->file_size || events_start_u64 > UINT32_MAX) {
      for (size_t j = 0; j < i; j++) {
        free_object_data(&objects[j]);
      }
      free(objects);
      return false;
    }
    uint32_t events_start = (uint32_t)events_start_u64;

    uint32_t event_type_count_u32 = 0;
    if (!read_u32_le(data, events_start, &event_type_count_u32)) {
      for (size_t j = 0; j < i; j++) {
        free_object_data(&objects[j]);
      }
      free(objects);
      return false;
    }
    size_t event_type_count = (size_t)event_type_count_u32;

    bs_object_event_list *events = NULL;
    if (event_type_count > 0) {
      events = (bs_object_event_list *)calloc(event_type_count, sizeof(bs_object_event_list));
      if (events == NULL) {
        for (size_t j = 0; j < i; j++) {
          free_object_data(&objects[j]);
        }
        free(objects);
        return false;
      }
    }

    for (size_t event_type = 0; event_type < event_type_count; event_type++) {
      uint32_t category_ptr = 0;
      if (!read_u32_le(data, events_start + 4 + (uint32_t)(event_type * 4), &category_ptr)) {
        for (size_t k = 0; k < event_type; k++) {
          for (size_t m = 0; m < events[k].entry_count; m++) {
            free(events[k].entries[m].actions);
          }
          free(events[k].entries);
        }
        free(events);
        for (size_t j = 0; j < i; j++) {
          free_object_data(&objects[j]);
        }
        free(objects);
        return false;
      }

      uint32_t sub_event_count_u32 = 0;
      if (!read_u32_le(data, category_ptr, &sub_event_count_u32)) {
        for (size_t k = 0; k < event_type; k++) {
          for (size_t m = 0; m < events[k].entry_count; m++) {
            free(events[k].entries[m].actions);
          }
          free(events[k].entries);
        }
        free(events);
        for (size_t j = 0; j < i; j++) {
          free_object_data(&objects[j]);
        }
        free(objects);
        return false;
      }

      size_t sub_event_count = (size_t)sub_event_count_u32;
      bs_event_entry *entries = NULL;
      if (sub_event_count > 0) {
        entries = (bs_event_entry *)calloc(sub_event_count, sizeof(bs_event_entry));
        if (entries == NULL) {
          for (size_t k = 0; k < event_type; k++) {
            for (size_t m = 0; m < events[k].entry_count; m++) {
              free(events[k].entries[m].actions);
            }
            free(events[k].entries);
          }
          free(events);
          for (size_t j = 0; j < i; j++) {
            free_object_data(&objects[j]);
          }
          free(objects);
          return false;
        }
      }

      for (size_t e = 0; e < sub_event_count; e++) {
        uint32_t event_ptr = 0;
        int32_t subtype = 0;
        uint32_t action_count_u32 = 0;
        if (!read_u32_le(data, category_ptr + 4 + (uint32_t)(e * 4), &event_ptr) ||
            !read_i32_le(data, event_ptr, &subtype) ||
            !read_u32_le(data, event_ptr + 4, &action_count_u32)) {
          for (size_t x = 0; x < e; x++) {
            free(entries[x].actions);
          }
          free(entries);
          for (size_t k = 0; k < event_type; k++) {
            for (size_t m = 0; m < events[k].entry_count; m++) {
              free(events[k].entries[m].actions);
            }
            free(events[k].entries);
          }
          free(events);
          for (size_t j = 0; j < i; j++) {
            free_object_data(&objects[j]);
          }
          free(objects);
          return false;
        }

        size_t action_count = (size_t)action_count_u32;
        bs_event_action *actions = NULL;
        if (action_count > 0) {
          actions = (bs_event_action *)calloc(action_count, sizeof(bs_event_action));
          if (actions == NULL) {
            for (size_t x = 0; x < e; x++) {
              free(entries[x].actions);
            }
            free(entries);
            for (size_t k = 0; k < event_type; k++) {
              for (size_t m = 0; m < events[k].entry_count; m++) {
                free(events[k].entries[m].actions);
              }
              free(events[k].entries);
            }
            free(events);
            for (size_t j = 0; j < i; j++) {
              free_object_data(&objects[j]);
            }
            free(objects);
            return false;
          }
        }

        for (size_t a = 0; a < action_count; a++) {
          uint32_t action_ptr = 0;
          int32_t raw_code_id = -1;
          if (!read_u32_le(data, event_ptr + 8 + (uint32_t)(a * 4), &action_ptr) ||
              !read_i32_le(data, action_ptr + 0x20, &raw_code_id)) {
            free(actions);
            for (size_t x = 0; x < e; x++) {
              free(entries[x].actions);
            }
            free(entries);
            for (size_t k = 0; k < event_type; k++) {
              for (size_t m = 0; m < events[k].entry_count; m++) {
                free(events[k].entries[m].actions);
              }
              free(events[k].entries);
            }
            free(events);
            for (size_t j = 0; j < i; j++) {
              free_object_data(&objects[j]);
            }
            free(objects);
            return false;
          }
          actions[a].code_id = resolve_code_index(data, raw_code_id);
        }

        entries[e].subtype = subtype;
        entries[e].actions = actions;
        entries[e].action_count = action_count;
      }

      events[event_type].entries = entries;
      events[event_type].entry_count = sub_event_count;
    }

    objects[i].name = read_string_ref(data, name_ptr);
    if (objects[i].name == NULL) {
      for (size_t k = 0; k < event_type_count; k++) {
        for (size_t m = 0; m < events[k].entry_count; m++) {
          free(events[k].entries[m].actions);
        }
        free(events[k].entries);
      }
      free(events);
      for (size_t j = 0; j < i; j++) {
        free_object_data(&objects[j]);
      }
      free(objects);
      return false;
    }
    objects[i].sprite_index = sprite_index;
    objects[i].visible = (visible_u32 != 0);
    objects[i].solid = (solid_u32 != 0);
    objects[i].depth = depth;
    objects[i].persistent = (persistent_u32 != 0);
    objects[i].parent_id = parent_id;
    objects[i].mask_id = mask_id;
    objects[i].events = events;
    objects[i].event_type_count = event_type_count;
  }

  data->objects = objects;
  data->object_count = count;
  printf("  OBJT: %u objects\n", count_u32);
  return true;
}

static bool parse_room(bs_game_data *data) {
  const bs_chunk_info *chunk = find_chunk(data, "ROOM");
  if (chunk == NULL) {
    return false;
  }

  uint32_t count_u32 = 0;
  if (!read_u32_le(data, chunk->data_offset, &count_u32)) {
    return false;
  }
  size_t count = (size_t)count_u32;

  if (!can_read_range(data->file_size, chunk->data_offset + 4, count * sizeof(uint32_t))) {
    return false;
  }

  bs_room_data *rooms = NULL;
  if (count > 0) {
    rooms = (bs_room_data *)calloc(count, sizeof(bs_room_data));
    if (rooms == NULL) {
      return false;
    }
  }

  for (size_t i = 0; i < count; i++) {
    uint32_t ptr = 0;
    uint32_t name_ptr = 0;
    uint32_t caption_ptr = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t speed = 0;
    uint32_t persistent_u32 = 0;
    uint32_t bg_color = 0;
    uint32_t draw_bg_color_u32 = 0;
    int32_t creation_code_raw = -1;
    uint32_t flags = 0;
    uint32_t bg_list_ptr = 0;
    uint32_t view_list_ptr = 0;
    uint32_t obj_list_ptr = 0;
    uint32_t tile_list_ptr = 0;

    if (!read_u32_le(data, chunk->data_offset + 4 + (uint32_t)(i * 4), &ptr) ||
        !read_u32_le(data, ptr + 0, &name_ptr) ||
        !read_u32_le(data, ptr + 4, &caption_ptr) ||
        !read_i32_le(data, ptr + 8, &width) ||
        !read_i32_le(data, ptr + 0x0C, &height) ||
        !read_i32_le(data, ptr + 0x10, &speed) ||
        !read_u32_le(data, ptr + 0x14, &persistent_u32) ||
        !read_u32_le(data, ptr + 0x18, &bg_color) ||
        !read_u32_le(data, ptr + 0x1C, &draw_bg_color_u32) ||
        !read_i32_le(data, ptr + 0x20, &creation_code_raw) ||
        !read_u32_le(data, ptr + 0x24, &flags) ||
        !read_u32_le(data, ptr + 0x28, &bg_list_ptr) ||
        !read_u32_le(data, ptr + 0x2C, &view_list_ptr) ||
        !read_u32_le(data, ptr + 0x30, &obj_list_ptr) ||
        !read_u32_le(data, ptr + 0x34, &tile_list_ptr)) {
      for (size_t j = 0; j < i; j++) {
        free_room_data(&rooms[j]);
      }
      free(rooms);
      return false;
    }

    uint32_t bg_count_u32 = 0;
    if (!read_u32_le(data, bg_list_ptr, &bg_count_u32)) {
      for (size_t j = 0; j < i; j++) {
        free_room_data(&rooms[j]);
      }
      free(rooms);
      return false;
    }
    size_t bg_count = (size_t)bg_count_u32;
    bs_room_background_data *backgrounds = NULL;
    if (bg_count > 0) {
      backgrounds = (bs_room_background_data *)calloc(bg_count, sizeof(bs_room_background_data));
      if (backgrounds == NULL) {
        for (size_t j = 0; j < i; j++) {
          free_room_data(&rooms[j]);
        }
        free(rooms);
        return false;
      }
    }
    for (size_t j = 0; j < bg_count; j++) {
      uint32_t bp = 0;
      uint32_t enabled = 0;
      uint32_t foreground = 0;
      int32_t bg_def_index = 0;
      int32_t x = 0;
      int32_t y = 0;
      uint32_t tile_x = 0;
      uint32_t tile_y = 0;
      int32_t speed_x = 0;
      int32_t speed_y = 0;
      uint32_t stretch = 0;
      if (!read_u32_le(data, bg_list_ptr + 4 + (uint32_t)(j * 4), &bp) ||
          !read_u32_le(data, bp + 0, &enabled) ||
          !read_u32_le(data, bp + 4, &foreground) ||
          !read_i32_le(data, bp + 8, &bg_def_index) ||
          !read_i32_le(data, bp + 12, &x) ||
          !read_i32_le(data, bp + 16, &y) ||
          !read_u32_le(data, bp + 20, &tile_x) ||
          !read_u32_le(data, bp + 24, &tile_y) ||
          !read_i32_le(data, bp + 28, &speed_x) ||
          !read_i32_le(data, bp + 32, &speed_y) ||
          !read_u32_le(data, bp + 36, &stretch)) {
        free(backgrounds);
        for (size_t k = 0; k < i; k++) {
          free_room_data(&rooms[k]);
        }
        free(rooms);
        return false;
      }
      backgrounds[j].enabled = (enabled != 0);
      backgrounds[j].foreground = (foreground != 0);
      backgrounds[j].bg_def_index = bg_def_index;
      backgrounds[j].x = x;
      backgrounds[j].y = y;
      backgrounds[j].tile_x = (tile_x != 0);
      backgrounds[j].tile_y = (tile_y != 0);
      backgrounds[j].speed_x = speed_x;
      backgrounds[j].speed_y = speed_y;
      backgrounds[j].stretch = (stretch != 0);
    }

    uint32_t view_count_u32 = 0;
    if (!read_u32_le(data, view_list_ptr, &view_count_u32)) {
      free(backgrounds);
      for (size_t k = 0; k < i; k++) {
        free_room_data(&rooms[k]);
      }
      free(rooms);
      return false;
    }
    size_t view_count = (size_t)view_count_u32;
    bs_room_view_data *views = NULL;
    if (view_count > 0) {
      views = (bs_room_view_data *)calloc(view_count, sizeof(bs_room_view_data));
      if (views == NULL) {
        free(backgrounds);
        for (size_t k = 0; k < i; k++) {
          free_room_data(&rooms[k]);
        }
        free(rooms);
        return false;
      }
    }
    for (size_t j = 0; j < view_count; j++) {
      uint32_t vp = 0;
      uint32_t enabled = 0;
      if (!read_u32_le(data, view_list_ptr + 4 + (uint32_t)(j * 4), &vp) ||
          !read_u32_le(data, vp + 0, &enabled) ||
          !read_i32_le(data, vp + 4, &views[j].view_x) ||
          !read_i32_le(data, vp + 8, &views[j].view_y) ||
          !read_i32_le(data, vp + 12, &views[j].view_w) ||
          !read_i32_le(data, vp + 16, &views[j].view_h) ||
          !read_i32_le(data, vp + 20, &views[j].port_x) ||
          !read_i32_le(data, vp + 24, &views[j].port_y) ||
          !read_i32_le(data, vp + 28, &views[j].port_w) ||
          !read_i32_le(data, vp + 32, &views[j].port_h) ||
          !read_i32_le(data, vp + 36, &views[j].border_h) ||
          !read_i32_le(data, vp + 40, &views[j].border_v) ||
          !read_i32_le(data, vp + 44, &views[j].speed_h) ||
          !read_i32_le(data, vp + 48, &views[j].speed_v) ||
          !read_i32_le(data, vp + 52, &views[j].follow_object_id)) {
        free(backgrounds);
        free(views);
        for (size_t k = 0; k < i; k++) {
          free_room_data(&rooms[k]);
        }
        free(rooms);
        return false;
      }
      views[j].enabled = (enabled != 0);
    }

    uint32_t instance_count_u32 = 0;
    if (!read_u32_le(data, obj_list_ptr, &instance_count_u32)) {
      free(backgrounds);
      free(views);
      for (size_t k = 0; k < i; k++) {
        free_room_data(&rooms[k]);
      }
      free(rooms);
      return false;
    }
    size_t instance_count = (size_t)instance_count_u32;
    bs_room_instance_data *instances = NULL;
    if (instance_count > 0) {
      instances = (bs_room_instance_data *)calloc(instance_count, sizeof(bs_room_instance_data));
      if (instances == NULL) {
        free(backgrounds);
        free(views);
        for (size_t k = 0; k < i; k++) {
          free_room_data(&rooms[k]);
        }
        free(rooms);
        return false;
      }
    }
    for (size_t j = 0; j < instance_count; j++) {
      uint32_t op = 0;
      int32_t creation_code_raw_inst = -1;
      if (!read_u32_le(data, obj_list_ptr + 4 + (uint32_t)(j * 4), &op) ||
          !read_i32_le(data, op + 0, &instances[j].x) ||
          !read_i32_le(data, op + 4, &instances[j].y) ||
          !read_i32_le(data, op + 8, &instances[j].object_def_id) ||
          !read_i32_le(data, op + 12, &instances[j].instance_id) ||
          !read_i32_le(data, op + 16, &creation_code_raw_inst) ||
          !read_f32_le(data, op + 20, &instances[j].scale_x) ||
          !read_f32_le(data, op + 24, &instances[j].scale_y) ||
          !read_u32_le(data, op + 28, &instances[j].color) ||
          !read_f32_le(data, op + 32, &instances[j].rotation)) {
        free(backgrounds);
        free(views);
        free(instances);
        for (size_t k = 0; k < i; k++) {
          free_room_data(&rooms[k]);
        }
        free(rooms);
        return false;
      }
      instances[j].creation_code_id =
          (creation_code_raw_inst >= 0) ? resolve_code_index(data, creation_code_raw_inst) : -1;
    }

    uint32_t tile_count_u32 = 0;
    if (!read_u32_le(data, tile_list_ptr, &tile_count_u32)) {
      free(backgrounds);
      free(views);
      free(instances);
      for (size_t k = 0; k < i; k++) {
        free_room_data(&rooms[k]);
      }
      free(rooms);
      return false;
    }
    size_t tile_count = (size_t)tile_count_u32;
    bs_room_tile_data *tiles = NULL;
    if (tile_count > 0) {
      tiles = (bs_room_tile_data *)calloc(tile_count, sizeof(bs_room_tile_data));
      if (tiles == NULL) {
        free(backgrounds);
        free(views);
        free(instances);
        for (size_t k = 0; k < i; k++) {
          free_room_data(&rooms[k]);
        }
        free(rooms);
        return false;
      }
    }
    for (size_t j = 0; j < tile_count; j++) {
      uint32_t tp = 0;
      if (!read_u32_le(data, tile_list_ptr + 4 + (uint32_t)(j * 4), &tp) ||
          !read_i32_le(data, tp + 0, &tiles[j].x) ||
          !read_i32_le(data, tp + 4, &tiles[j].y) ||
          !read_i32_le(data, tp + 8, &tiles[j].bg_def_index) ||
          !read_i32_le(data, tp + 12, &tiles[j].source_x) ||
          !read_i32_le(data, tp + 16, &tiles[j].source_y) ||
          !read_i32_le(data, tp + 20, &tiles[j].width) ||
          !read_i32_le(data, tp + 24, &tiles[j].height) ||
          !read_i32_le(data, tp + 28, &tiles[j].depth) ||
          !read_i32_le(data, tp + 32, &tiles[j].instance_id) ||
          !read_f32_le(data, tp + 36, &tiles[j].scale_x) ||
          !read_f32_le(data, tp + 40, &tiles[j].scale_y) ||
          !read_u32_le(data, tp + 44, &tiles[j].color)) {
        free(backgrounds);
        free(views);
        free(instances);
        free(tiles);
        for (size_t k = 0; k < i; k++) {
          free_room_data(&rooms[k]);
        }
        free(rooms);
        return false;
      }
    }

    rooms[i].name = read_string_ref(data, name_ptr);
    rooms[i].caption = read_string_ref(data, caption_ptr);
    if (rooms[i].name == NULL || rooms[i].caption == NULL) {
      free(rooms[i].name);
      free(rooms[i].caption);
      free(backgrounds);
      free(views);
      free(instances);
      free(tiles);
      for (size_t k = 0; k < i; k++) {
        free_room_data(&rooms[k]);
      }
      free(rooms);
      return false;
    }

    rooms[i].width = width;
    rooms[i].height = height;
    rooms[i].speed = speed;
    rooms[i].persistent = (persistent_u32 != 0);
    rooms[i].bg_color = bg_color;
    rooms[i].draw_bg_color = (draw_bg_color_u32 != 0);
    rooms[i].creation_code_id =
        (creation_code_raw >= 0) ? resolve_code_index(data, creation_code_raw) : -1;
    rooms[i].flags = flags;
    rooms[i].backgrounds = backgrounds;
    rooms[i].background_count = bg_count;
    rooms[i].views = views;
    rooms[i].view_count = view_count;
    rooms[i].instances = instances;
    rooms[i].instance_count = instance_count;
    rooms[i].tiles = tiles;
    rooms[i].tile_count = tile_count;
  }

  data->rooms = rooms;
  data->room_count = count;
  printf("  ROOM: %u rooms\n", count_u32);
  return true;
}

bool bs_form_reader_read(const char *path, bs_game_data *out_data) {
  if (path == NULL || out_data == NULL) {
    return false;
  }

  memset(out_data, 0, sizeof(*out_data));
  (void)snprintf(out_data->game_path, sizeof(out_data->game_path), "%s", path);

  if (!load_file_bytes(path, &out_data->file_data, &out_data->file_size)) {
    return false;
  }
  if (!discover_chunks(out_data)) {
    bs_game_data_free(out_data);
    return false;
  }

  print_chunk_list(out_data);

  if (!parse_strg(out_data) ||
      !parse_gen8(out_data) ||
      !parse_tpag(out_data) ||
      !parse_txtr(out_data) ||
      !parse_sprt(out_data) ||
      !parse_bgnd(out_data) ||
      !parse_path(out_data) ||
      !parse_font(out_data) ||
      !parse_sond(out_data) ||
      !parse_audo(out_data) ||
      !parse_code(out_data) ||
      !parse_objt(out_data) ||
      !parse_room(out_data) ||
      !parse_scpt(out_data) ||
      !parse_vari(out_data) ||
      !parse_func(out_data)) {
    bs_game_data_free(out_data);
    return false;
  }

  printf("Loaded bootstrap: SPRT=%zu, BGND=%zu, PATH=%zu, FONT=%zu, OBJT=%zu, ROOM=%zu, CODE=%zu, VARI=%zu, FUNC=%zu, SCPT=%zu, SOND=%zu, AUDO=%zu\n",
         out_data->sprite_count,
         out_data->background_count,
         out_data->path_count,
         out_data->font_count,
         out_data->object_count,
         out_data->room_count,
         out_data->code_entry_count,
         out_data->variable_count,
         out_data->function_count,
         out_data->script_count,
         out_data->sound_count,
         out_data->audio_data_count);

  return true;
}

void bs_game_data_free(bs_game_data *data) {
  if (data == NULL) {
    return;
  }

  for (size_t i = 0; i < data->string_count; i++) {
    free(data->strings[i]);
  }
  free(data->strings);
  data->strings = NULL;
  data->string_count = 0;

  free(data->gen8.game_name);
  free(data->gen8.display_name);
  free(data->gen8.room_order);
  data->gen8.game_name = NULL;
  data->gen8.display_name = NULL;
  data->gen8.room_order = NULL;
  data->gen8.room_order_count = 0;
  data->gen8.bytecode_version = 0;
  data->gen8.game_id = 0;
  data->gen8.window_width = 0;
  data->gen8.window_height = 0;

  free(data->texture_page_items);
  free(data->texture_page_item_offsets);
  data->texture_page_items = NULL;
  data->texture_page_item_offsets = NULL;
  data->texture_page_item_count = 0;

  free(data->texture_pages);
  data->texture_pages = NULL;
  data->texture_page_count = 0;

  for (size_t i = 0; i < data->sprite_count; i++) {
    free_sprite_data(&data->sprites[i]);
  }
  free(data->sprites);
  data->sprites = NULL;
  data->sprite_count = 0;

  for (size_t i = 0; i < data->background_count; i++) {
    free_background_data(&data->backgrounds[i]);
  }
  free(data->backgrounds);
  data->backgrounds = NULL;
  data->background_count = 0;

  for (size_t i = 0; i < data->path_count; i++) {
    free_path_data(&data->paths[i]);
  }
  free(data->paths);
  data->paths = NULL;
  data->path_count = 0;

  for (size_t i = 0; i < data->font_count; i++) {
    free_font_data(&data->fonts[i]);
  }
  free(data->fonts);
  data->fonts = NULL;
  data->font_count = 0;

  for (size_t i = 0; i < data->code_entry_count; i++) {
    free(data->code_entries[i].name);
    free(data->code_entries[i].bytecode);
  }
  free(data->code_entries);
  data->code_entries = NULL;
  data->code_entry_count = 0;

  for (size_t i = 0; i < data->sound_count; i++) {
    free(data->sounds[i].name);
    free(data->sounds[i].extension);
    free(data->sounds[i].file_name);
  }
  free(data->sounds);
  data->sounds = NULL;
  data->sound_count = 0;

  free(data->audio_data);
  data->audio_data = NULL;
  data->audio_data_count = 0;

  for (size_t i = 0; i < data->script_count; i++) {
    free(data->scripts[i].name);
  }
  free(data->scripts);
  data->scripts = NULL;
  data->script_count = 0;

  for (size_t i = 0; i < data->variable_count; i++) {
    free(data->variables[i].name);
  }
  free(data->variables);
  data->variables = NULL;
  data->variable_count = 0;

  for (size_t i = 0; i < data->function_count; i++) {
    free(data->functions[i].name);
  }
  free(data->functions);
  data->functions = NULL;
  data->function_count = 0;

  for (size_t i = 0; i < data->object_count; i++) {
    free_object_data(&data->objects[i]);
  }
  free(data->objects);
  data->objects = NULL;
  data->object_count = 0;

  for (size_t i = 0; i < data->room_count; i++) {
    free_room_data(&data->rooms[i]);
  }
  free(data->rooms);
  data->rooms = NULL;
  data->room_count = 0;

  free(data->chunks);
  data->chunks = NULL;
  data->chunk_count = 0;
  data->form_size = 0;

  free(data->file_data);
  data->file_data = NULL;
  data->file_size = 0;

  data->game_path[0] = '\0';
}
