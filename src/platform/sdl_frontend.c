#include "bs/platform/sdl_frontend.h"

#include "bs/builtin/builtin_registry.h"
#include "bs/data/form_reader.h"
#include "bs/runtime/game_runner.h"
#include "bs/vm/vm.h"

#if defined(__has_include)
#if __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#else
#error "SDL headers not found"
#endif
#else
#include <SDL.h>
#endif

#if defined(BS_HAVE_SDL_IMAGE)
#if defined(__has_include)
#if __has_include(<SDL2/SDL_image.h>)
#include <SDL2/SDL_image.h>
#elif __has_include(<SDL_image.h>)
#include <SDL_image.h>
#endif
#else
#include <SDL_image.h>
#endif
#endif

#if defined(BS_HAVE_SDL_MIXER)
#if defined(__has_include)
#if __has_include(<SDL2/SDL_mixer.h>)
#include <SDL2/SDL_mixer.h>
#elif __has_include(<SDL_mixer.h>)
#include <SDL_mixer.h>
#endif
#else
#include <SDL_mixer.h>
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

typedef struct bs_sdl_draw_context {
  SDL_Renderer *renderer;
  const bs_game_data *game_data;
  SDL_Texture **texture_pages;
  size_t texture_page_count;
  bool texture_pages_ready;
} bs_sdl_draw_context;

static void bs_sdl_unpack_color(int32_t gm_color,
                                uint8_t alpha,
                                uint8_t *out_r,
                                uint8_t *out_g,
                                uint8_t *out_b,
                                uint8_t *out_a) {
  uint32_t c = (uint32_t)gm_color & 0xFFFFFFu;
  if (out_r != NULL) {
    *out_r = (uint8_t)(c & 0xFFu);
  }
  if (out_g != NULL) {
    *out_g = (uint8_t)((c >> 8) & 0xFFu);
  }
  if (out_b != NULL) {
    *out_b = (uint8_t)((c >> 16) & 0xFFu);
  }
  if (out_a != NULL) {
    *out_a = alpha;
  }
}

static uint8_t bs_sdl_alpha_from_unit(double alpha) {
  if (alpha <= 0.0) {
    return 0;
  }
  if (alpha >= 1.0) {
    return 255;
  }
  return (uint8_t)(alpha * 255.0);
}

static int bs_sdl_floor_div(int num, int den) {
  int q = 0;
  int r = 0;
  if (den == 0) {
    return 0;
  }
  q = num / den;
  r = num % den;
  if (r != 0 && ((r < 0) != (den < 0))) {
    q -= 1;
  }
  return q;
}

static const bs_room_view_data *bs_sdl_choose_enabled_view(const bs_game_runner *runner) {
  const bs_room_data *room = NULL;
  if (runner == NULL || runner->current_room == NULL) {
    return NULL;
  }
  room = runner->current_room;
  if (room->views == NULL || room->view_count == 0) {
    return NULL;
  }
  for (size_t i = 0; i < room->view_count; i++) {
    const bs_room_view_data *view = &room->views[i];
    if (view->enabled && view->view_w > 0 && view->view_h > 0) {
      return view;
    }
  }
  return NULL;
}

static void bs_sdl_world_to_screen(const bs_game_runner *runner,
                                   double world_x,
                                   double world_y,
                                   double *out_screen_x,
                                   double *out_screen_y) {
  const bs_room_view_data *view = NULL;
  double screen_x = world_x;
  double screen_y = world_y;
  bool is_gui_event = false;
  
  /* Check if we're in a GUI draw event (subtype 64, 73, or 74) */
  /* GUI events use screen coordinates and should not be transformed by the view */
  if (runner != NULL && runner->event_context_active && runner->current_event_type == 8) {
    if (runner->current_event_subtype == 64 || 
        runner->current_event_subtype == 73 || 
        runner->current_event_subtype == 74) {
      is_gui_event = true;
    }
  }
  
  /* Only apply view transformation for non-GUI events */
  if (!is_gui_event) {
    view = bs_sdl_choose_enabled_view(runner);
    if (view != NULL) {
      screen_x += (double)(view->port_x - view->view_x);
      screen_y += (double)(view->port_y - view->view_y);
    }
  }
  
  if (out_screen_x != NULL) {
    *out_screen_x = screen_x;
  }
  if (out_screen_y != NULL) {
    *out_screen_y = screen_y;
  }
}

#if defined(BS_HAVE_SDL_IMAGE)
static void bs_sdl_free_texture_pages(bs_sdl_draw_context *ctx) {
  if (ctx == NULL || ctx->texture_pages == NULL) {
    return;
  }
  for (size_t i = 0; i < ctx->texture_page_count; i++) {
    if (ctx->texture_pages[i] != NULL) {
      SDL_DestroyTexture(ctx->texture_pages[i]);
    }
  }
  free(ctx->texture_pages);
  ctx->texture_pages = NULL;
  ctx->texture_page_count = 0;
  ctx->texture_pages_ready = false;
}

static bool bs_sdl_load_texture_pages(bs_sdl_draw_context *ctx, const bs_game_data *game_data) {
  size_t loaded_count = 0;
  if (ctx == NULL || game_data == NULL || ctx->renderer == NULL || game_data->texture_page_count == 0) {
    return false;
  }

  ctx->texture_pages = (SDL_Texture **)calloc(game_data->texture_page_count, sizeof(SDL_Texture *));
  if (ctx->texture_pages == NULL) {
    return false;
  }
  ctx->texture_page_count = game_data->texture_page_count;

  for (size_t i = 0; i < game_data->texture_page_count; i++) {
    const bs_texture_page_data *page = &game_data->texture_pages[i];
    const uint8_t *bytes = NULL;
    SDL_RWops *rw = NULL;
    SDL_Surface *surface = NULL;
    SDL_Texture *texture = NULL;
    if (page->png_length == 0) {
      continue;
    }
    if ((size_t)page->png_offset >= game_data->file_size) {
      continue;
    }
    if ((size_t)page->png_length > (game_data->file_size - (size_t)page->png_offset)) {
      continue;
    }

    bytes = &game_data->file_data[page->png_offset];
    rw = SDL_RWFromConstMem(bytes, (int)page->png_length);
    if (rw == NULL) {
      continue;
    }
    surface = IMG_Load_RW(rw, 1);
    if (surface == NULL) {
      continue;
    }
    texture = SDL_CreateTextureFromSurface(ctx->renderer, surface);
    SDL_FreeSurface(surface);
    if (texture == NULL) {
      continue;
    }
    (void)SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    ctx->texture_pages[i] = texture;
    loaded_count++;
  }

  ctx->texture_pages_ready = loaded_count > 0;
  return ctx->texture_pages_ready;
}
#else
static void bs_sdl_free_texture_pages(bs_sdl_draw_context *ctx) {
  if (ctx == NULL) {
    return;
  }
  free(ctx->texture_pages);
  ctx->texture_pages = NULL;
  ctx->texture_page_count = 0;
  ctx->texture_pages_ready = false;
}

static bool bs_sdl_load_texture_pages(bs_sdl_draw_context *ctx, const bs_game_data *game_data) {
  (void)ctx;
  (void)game_data;
  return false;
}
#endif

static size_t bs_sdl_utf8_decode(const char *s, size_t remaining, uint32_t *out_codepoint) {
  uint8_t b0 = 0;
  if (s == NULL || remaining == 0 || out_codepoint == NULL) {
    return 0;
  }
  b0 = (uint8_t)s[0];
  if (b0 < 0x80u) {
    *out_codepoint = b0;
    return 1;
  }
  if ((b0 & 0xE0u) == 0xC0u && remaining >= 2) {
    uint8_t b1 = (uint8_t)s[1];
    if ((b1 & 0xC0u) == 0x80u) {
      *out_codepoint = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(b1 & 0x3Fu);
      return 2;
    }
  }
  if ((b0 & 0xF0u) == 0xE0u && remaining >= 3) {
    uint8_t b1 = (uint8_t)s[1];
    uint8_t b2 = (uint8_t)s[2];
    if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u) {
      *out_codepoint = ((uint32_t)(b0 & 0x0Fu) << 12) |
                       ((uint32_t)(b1 & 0x3Fu) << 6) |
                       (uint32_t)(b2 & 0x3Fu);
      return 3;
    }
  }
  if ((b0 & 0xF8u) == 0xF0u && remaining >= 4) {
    uint8_t b1 = (uint8_t)s[1];
    uint8_t b2 = (uint8_t)s[2];
    uint8_t b3 = (uint8_t)s[3];
    if ((b1 & 0xC0u) == 0x80u && (b2 & 0xC0u) == 0x80u && (b3 & 0xC0u) == 0x80u) {
      *out_codepoint = ((uint32_t)(b0 & 0x07u) << 18) |
                       ((uint32_t)(b1 & 0x3Fu) << 12) |
                       ((uint32_t)(b2 & 0x3Fu) << 6) |
                       (uint32_t)(b3 & 0x3Fu);
      return 4;
    }
  }
  *out_codepoint = 0xFFFDu;
  return 1;
}

static const bs_font_glyph_data *bs_sdl_find_glyph_codepoint(const bs_font_data *font, uint32_t codepoint) {
  uint16_t glyph_code = 0;
  if (font == NULL || font->glyphs == NULL) {
    return NULL;
  }
  glyph_code = (codepoint <= 0xFFFFu) ? (uint16_t)codepoint : 0u;
  for (size_t i = 0; i < font->glyph_count; i++) {
    if (font->glyphs[i].character == glyph_code) {
      return &font->glyphs[i];
    }
  }
  return NULL;
}

static double bs_sdl_measure_line_width(const bs_font_data *font,
                                        const char *line,
                                        size_t line_len,
                                        double xscale) {
  double width = 0.0;
  size_t at = 0;
  if (line == NULL) {
    return 0.0;
  }
  if (xscale <= 0.0) {
    xscale = 1.0;
  }
  while (at < line_len) {
    uint32_t cp = 0;
    size_t consumed = bs_sdl_utf8_decode(line + at, line_len - at, &cp);
    const bs_font_glyph_data *glyph = NULL;
    if (consumed == 0) {
      break;
    }
    at += consumed;
    if (cp == '\r') {
      continue;
    }
    glyph = bs_sdl_find_glyph_codepoint(font, cp);
    if (glyph != NULL) {
      width += ((double)(int16_t)glyph->shift) * xscale;
    } else {
      width += 6.0 * xscale;
    }
  }
  return width;
}

static bool bs_sdl_get_tpag_texture(const bs_sdl_draw_context *ctx,
                                    int32_t tpag_index,
                                    const bs_texture_page_item_data **out_tpag,
                                    SDL_Texture **out_texture) {
  const bs_texture_page_item_data *tpag = NULL;
  SDL_Texture *texture = NULL;
  if (ctx == NULL || ctx->game_data == NULL || tpag_index < 0) {
    return false;
  }
  if ((size_t)tpag_index >= ctx->game_data->texture_page_item_count ||
      ctx->game_data->texture_page_items == NULL) {
    return false;
  }
  tpag = &ctx->game_data->texture_page_items[(size_t)tpag_index];
  if (!ctx->texture_pages_ready ||
      ctx->texture_pages == NULL ||
      tpag->texture_page_id >= ctx->texture_page_count) {
    return false;
  }
  texture = ctx->texture_pages[tpag->texture_page_id];
  if (texture == NULL) {
    return false;
  }
  if (out_tpag != NULL) {
    *out_tpag = tpag;
  }
  if (out_texture != NULL) {
    *out_texture = texture;
  }
  return true;
}

static void bs_sdl_clear(void *userdata, int32_t bg_color) {
  bs_sdl_draw_context *ctx = (bs_sdl_draw_context *)userdata;
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 255;
  if (ctx == NULL || ctx->renderer == NULL) {
    return;
  }
  bs_sdl_unpack_color(bg_color, 255, &r, &g, &b, &a);
  (void)SDL_SetRenderDrawColor(ctx->renderer, r, g, b, 255);
  (void)SDL_RenderClear(ctx->renderer);
}

static void bs_sdl_draw_background(void *userdata,
                                   const bs_game_runner *runner,
                                   int32_t tpag_index,
                                   int32_t x,
                                   int32_t y,
                                   bool tile_x,
                                   bool tile_y) {
  bs_sdl_draw_context *ctx = (bs_sdl_draw_context *)userdata;
  const bs_texture_page_item_data *tpag = NULL;
  SDL_Texture *texture = NULL;
  const bs_room_view_data *view = NULL;
  int world_left = 0;
  int world_top = 0;
  int world_right = 640;
  int world_bottom = 480;
  int screen_offset_x = 0;
  int screen_offset_y = 0;
  if (ctx == NULL || ctx->renderer == NULL || runner == NULL) {
    return;
  }
#if defined(BS_HAVE_SDL_IMAGE)
  if (!bs_sdl_get_tpag_texture(ctx, tpag_index, &tpag, &texture)) {
    return;
  }
  world_right = runner->surface_width > 0 ? runner->surface_width : 640;
  world_bottom = runner->surface_height > 0 ? runner->surface_height : 480;
  view = bs_sdl_choose_enabled_view(runner);
  if (view != NULL) {
    world_left = view->view_x;
    world_top = view->view_y;
    world_right = view->view_x + (view->view_w > 0 ? view->view_w : world_right);
    world_bottom = view->view_y + (view->view_h > 0 ? view->view_h : world_bottom);
    screen_offset_x = view->port_x - view->view_x;
    screen_offset_y = view->port_y - view->view_y;
  }
  if (world_right <= world_left) {
    world_right = world_left + (runner->surface_width > 0 ? runner->surface_width : 640);
  }
  if (world_bottom <= world_top) {
    world_bottom = world_top + (runner->surface_height > 0 ? runner->surface_height : 480);
  }
  {
    SDL_Rect src = {0};
    src.x = (int)tpag->source_x;
    src.y = (int)tpag->source_y;
    src.w = (int)tpag->source_width;
    src.h = (int)tpag->source_height;
    if (src.w <= 0 || src.h <= 0) {
      return;
    }
    (void)SDL_SetTextureColorMod(texture, 255, 255, 255);
    (void)SDL_SetTextureAlphaMod(texture, 255);
    if (tile_x || tile_y) {
      int start_x = x;
      int start_y = y;
      int end_x = tile_x ? world_right : (x + src.w);
      int end_y = tile_y ? world_bottom : (y + src.h);
      if (tile_x) {
        start_x = x + (bs_sdl_floor_div(world_left - x, src.w) * src.w);
      }
      if (tile_y) {
        start_y = y + (bs_sdl_floor_div(world_top - y, src.h) * src.h);
      }
      for (int cy = start_y; cy < end_y; cy += src.h) {
        for (int cx = start_x; cx < end_x; cx += src.w) {
          SDL_Rect dst = {cx + screen_offset_x, cy + screen_offset_y, src.w, src.h};
          (void)SDL_RenderCopy(ctx->renderer, texture, &src, &dst);
        }
      }
    } else {
      SDL_Rect dst = {x + screen_offset_x, y + screen_offset_y, src.w, src.h};
      (void)SDL_RenderCopy(ctx->renderer, texture, &src, &dst);
    }
  }
#else
  (void)tpag_index;
  (void)x;
  (void)y;
  (void)tile_x;
  (void)tile_y;
#endif
}

static void bs_sdl_draw_tile(void *userdata,
                             const bs_game_runner *runner,
                             int32_t tpag_index,
                             int32_t x,
                             int32_t y,
                             int32_t source_x,
                             int32_t source_y,
                             int32_t width,
                             int32_t height,
                             double scale_x,
                             double scale_y,
                             int32_t color) {
  bs_sdl_draw_context *ctx = (bs_sdl_draw_context *)userdata;
  const bs_texture_page_item_data *tpag = NULL;
  SDL_Texture *texture = NULL;
  (void)runner;
  if (ctx == NULL || ctx->renderer == NULL || width <= 0 || height <= 0) {
    return;
  }
  if (scale_x == 0.0 || scale_y == 0.0) {
    return;
  }
#if defined(BS_HAVE_SDL_IMAGE)
  if (!bs_sdl_get_tpag_texture(ctx, tpag_index, &tpag, &texture)) {
    return;
  }
  {
    uint32_t argb = (uint32_t)color;
    uint8_t a = (uint8_t)((argb >> 24) & 0xFFu);
    uint8_t r = (uint8_t)((argb >> 16) & 0xFFu);
    uint8_t g = (uint8_t)((argb >> 8) & 0xFFu);
    uint8_t b = (uint8_t)(argb & 0xFFu);
      SDL_Rect src = {0};
      SDL_Rect dst = {0};
      SDL_RendererFlip flip = SDL_FLIP_NONE;
      double dw = (double)width * scale_x;
      double dh = (double)height * scale_y;
      double dx = (double)x;
      double dy = (double)y;
      bs_sdl_world_to_screen(runner, dx, dy, &dx, &dy);
      src.x = (int)tpag->source_x + source_x;
      src.y = (int)tpag->source_y + source_y;
      src.w = width;
      src.h = height;
    if (dw < 0.0) {
      flip = (SDL_RendererFlip)(flip | SDL_FLIP_HORIZONTAL);
      dx += dw;
      dw = -dw;
    }
    if (dh < 0.0) {
      flip = (SDL_RendererFlip)(flip | SDL_FLIP_VERTICAL);
      dy += dh;
      dh = -dh;
    }
    dst.x = (int)lround(dx);
    dst.y = (int)lround(dy);
    dst.w = (int)lround(dw);
    dst.h = (int)lround(dh);
    if (dst.w < 1) {
      dst.w = 1;
    }
    if (dst.h < 1) {
      dst.h = 1;
    }
    (void)SDL_SetTextureColorMod(texture, r, g, b);
    (void)SDL_SetTextureAlphaMod(texture, a);
    (void)SDL_RenderCopyEx(ctx->renderer, texture, &src, &dst, 0.0, NULL, flip);
  }
#else
  (void)tpag_index;
  (void)x;
  (void)y;
  (void)source_x;
  (void)source_y;
  (void)width;
  (void)height;
  (void)scale_x;
  (void)scale_y;
  (void)color;
#endif
}

static bool bs_sdl_get_sprite_frame(const bs_sdl_draw_context *ctx,
                                    int32_t sprite_index,
                                    int32_t image_index,
                                    const bs_sprite_data **out_sprite,
                                    const bs_texture_page_item_data **out_tpag,
                                    SDL_Texture **out_texture) {
  int32_t frame_index = 0;
  int32_t tpag_index = -1;
  const bs_sprite_data *sprite = NULL;
  const bs_texture_page_item_data *tpag = NULL;
  SDL_Texture *texture = NULL;
  if (ctx == NULL || ctx->game_data == NULL) {
    return false;
  }
  if (sprite_index < 0 || (size_t)sprite_index >= ctx->game_data->sprite_count) {
    return false;
  }
  sprite = &ctx->game_data->sprites[(size_t)sprite_index];
  if (sprite->tpag_indices == NULL || sprite->subimage_count == 0) {
    return false;
  }
  frame_index = image_index;
  if (frame_index < 0) {
    frame_index = (int32_t)(frame_index % (int32_t)sprite->subimage_count);
    if (frame_index < 0) {
      frame_index += (int32_t)sprite->subimage_count;
    }
  } else if ((size_t)frame_index >= sprite->subimage_count) {
    frame_index = (int32_t)(frame_index % (int32_t)sprite->subimage_count);
  }
  tpag_index = sprite->tpag_indices[(size_t)frame_index];
  if (tpag_index < 0 || (size_t)tpag_index >= ctx->game_data->texture_page_item_count) {
    return false;
  }
  if (!bs_sdl_get_tpag_texture(ctx, tpag_index, &tpag, &texture)) {
    return false;
  }
  if (out_sprite != NULL) {
    *out_sprite = sprite;
  }
  if (out_tpag != NULL) {
    *out_tpag = tpag;
  }
  if (out_texture != NULL) {
    *out_texture = texture;
  }
  return true;
}

static void bs_sdl_draw_sprite_ext(void *userdata,
                                   const bs_game_runner *runner,
                                   int32_t sprite_index,
                                   int32_t image_index,
                                   double x,
                                   double y,
                                   double xscale,
                                   double yscale,
                                   double angle,
                                   int32_t blend_color,
                                   double alpha) {
  bs_sdl_draw_context *ctx = (bs_sdl_draw_context *)userdata;
  const bs_sprite_data *sprite = NULL;
  const bs_texture_page_item_data *tpag = NULL;
  SDL_Texture *texture = NULL;
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 255;
  double draw_x = x;
  double draw_y = y;
  if (ctx == NULL || ctx->renderer == NULL || ctx->game_data == NULL || runner == NULL) {
    return;
  }
  if (sprite_index < 0 || (size_t)sprite_index >= ctx->game_data->sprite_count) {
    return;
  }
  sprite = &ctx->game_data->sprites[(size_t)sprite_index];
  if (xscale == 0.0 || yscale == 0.0) {
    return;
  }
  bs_sdl_world_to_screen(runner, x, y, &draw_x, &draw_y);

  bs_sdl_unpack_color(blend_color, bs_sdl_alpha_from_unit(alpha), &r, &g, &b, &a);
#if defined(BS_HAVE_SDL_IMAGE)
  if (bs_sdl_get_sprite_frame(ctx, sprite_index, image_index, &sprite, &tpag, &texture)) {
    SDL_Rect src = {0};
    SDL_Rect dst = {0};
    SDL_Point center = {0};
    SDL_RendererFlip flip = SDL_FLIP_NONE;
    double sx = xscale;
    double sy = yscale;
    double origin_x = (double)sprite->origin_x;
    double origin_y = (double)sprite->origin_y;
    double tgt_x = (double)tpag->target_x;
    double tgt_y = (double)tpag->target_y;
    double src_w = (double)tpag->source_width;
    double src_h = (double)tpag->source_height;
    /* In GameMaker, position = draw_pos - origin * scale + target * scale
       The texture region (source) is drawn at an offset of target within the
       bounding box, and the bounding box is positioned with the origin.
       When scale is negative, the sprite flips around the origin. */
    double top_left_x = draw_x + (-origin_x + tgt_x) * sx;
    double top_left_y = draw_y + (-origin_y + tgt_y) * sy;
    double rect_w = src_w * sx;
    double rect_h = src_h * sy;
    /* If scale is negative, the rect extends in the opposite direction.
       SDL needs positive width/height, so flip the rect and use SDL_FLIP. */
    if (rect_w < 0.0) {
      top_left_x += rect_w;
      rect_w = -rect_w;
      flip = (SDL_RendererFlip)(flip | SDL_FLIP_HORIZONTAL);
    }
    if (rect_h < 0.0) {
      top_left_y += rect_h;
      rect_h = -rect_h;
      flip = (SDL_RendererFlip)(flip | SDL_FLIP_VERTICAL);
    }
    src.x = (int)tpag->source_x;
    src.y = (int)tpag->source_y;
    src.w = (int)tpag->source_width;
    src.h = (int)tpag->source_height;
    dst.x = (int)lround(top_left_x);
    dst.y = (int)lround(top_left_y);
    dst.w = (int)lround(rect_w);
    dst.h = (int)lround(rect_h);
    if (dst.w < 1) {
      dst.w = 1;
    }
    if (dst.h < 1) {
      dst.h = 1;
    }
    /* Center of rotation is at the origin position within the dst rect */
    center.x = (int)lround((origin_x - tgt_x) * fabs(sx));
    center.y = (int)lround((origin_y - tgt_y) * fabs(sy));
    if (flip & SDL_FLIP_HORIZONTAL) {
      center.x = dst.w - center.x;
    }
    if (flip & SDL_FLIP_VERTICAL) {
      center.y = dst.h - center.y;
    }
    (void)SDL_SetTextureColorMod(texture, r, g, b);
    (void)SDL_SetTextureAlphaMod(texture, a);
    (void)SDL_RenderCopyEx(ctx->renderer, texture, &src, &dst, -angle, &center, flip);
    return;
  }
#endif

  {
    SDL_Rect rect = {0};
    int w = (int)lround((double)(sprite->width > 0 ? sprite->width : 1) * fabs(xscale));
    int h = (int)lround((double)(sprite->height > 0 ? sprite->height : 1) * fabs(yscale));
    if (w < 1) {
      w = 1;
    }
    if (h < 1) {
      h = 1;
    }
    rect.x = (int)lround(draw_x - (double)sprite->origin_x * fabs(xscale));
    rect.y = (int)lround(draw_y - (double)sprite->origin_y * fabs(yscale));
    rect.w = w;
    rect.h = h;
    if (r == 0 && g == 0 && b == 0) {
      uint32_t hsh = (uint32_t)sprite_index * 2654435761u;
      r = (uint8_t)(64 + (hsh & 0x7F));
      g = (uint8_t)(64 + ((hsh >> 8) & 0x7F));
      b = (uint8_t)(64 + ((hsh >> 16) & 0x7F));
    }
    (void)SDL_SetRenderDrawColor(ctx->renderer, r, g, b, a);
    (void)SDL_RenderFillRect(ctx->renderer, &rect);
  }
}

static void bs_sdl_draw_sprite_part_ext(void *userdata,
                                        const bs_game_runner *runner,
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
                                        double alpha) {
  bs_sdl_draw_context *ctx = (bs_sdl_draw_context *)userdata;
  const bs_sprite_data *sprite = NULL;
  const bs_texture_page_item_data *tpag = NULL;
  SDL_Texture *texture = NULL;
  uint8_t r = 255;
  uint8_t g = 255;
  uint8_t b = 255;
  uint8_t a = 255;
  double draw_x = x;
  double draw_y = y;
  if (ctx == NULL || ctx->renderer == NULL || runner == NULL) {
    return;
  }
  if (xscale == 0.0 || yscale == 0.0 || width <= 0 || height <= 0) {
    return;
  }
  bs_sdl_world_to_screen(runner, x, y, &draw_x, &draw_y);
  bs_sdl_unpack_color(blend_color, bs_sdl_alpha_from_unit(alpha), &r, &g, &b, &a);

#if defined(BS_HAVE_SDL_IMAGE)
  if (bs_sdl_get_sprite_frame(ctx, sprite_index, image_index, &sprite, &tpag, &texture)) {
    int32_t target_x = (int32_t)tpag->target_x;
    int32_t target_y = (int32_t)tpag->target_y;
    int32_t source_x = (int32_t)tpag->source_x;
    int32_t source_y = (int32_t)tpag->source_y;
    int32_t source_w = (int32_t)tpag->source_width;
    int32_t source_h = (int32_t)tpag->source_height;
    int32_t clip_left = left > target_x ? left : target_x;
    int32_t clip_top = top > target_y ? top : target_y;
    int32_t clip_right = (left + width) < (target_x + source_w) ? (left + width) : (target_x + source_w);
    int32_t clip_bottom = (top + height) < (target_y + source_h) ? (top + height) : (target_y + source_h);
    int32_t clipped_w = 0;
    int32_t clipped_h = 0;
    if (clip_left >= clip_right || clip_top >= clip_bottom) {
      return;
    }
    clipped_w = clip_right - clip_left;
    clipped_h = clip_bottom - clip_top;
    if (clipped_w <= 0 || clipped_h <= 0) {
      return;
    }

    {
      SDL_Rect src = {0};
      SDL_Rect dst = {0};
      SDL_RendererFlip flip = SDL_FLIP_NONE;
      double dx = draw_x + (double)(clip_left - left) * xscale;
      double dy = draw_y + (double)(clip_top - top) * yscale;
      double dw = (double)clipped_w * xscale;
      double dh = (double)clipped_h * yscale;
      if (dw < 0.0) {
        flip = (SDL_RendererFlip)(flip | SDL_FLIP_HORIZONTAL);
        dx += dw;
        dw = -dw;
      }
      if (dh < 0.0) {
        flip = (SDL_RendererFlip)(flip | SDL_FLIP_VERTICAL);
        dy += dh;
        dh = -dh;
      }
      src.x = source_x + (clip_left - target_x);
      src.y = source_y + (clip_top - target_y);
      src.w = clipped_w;
      src.h = clipped_h;
      dst.x = (int)lround(dx);
      dst.y = (int)lround(dy);
      dst.w = (int)lround(dw);
      dst.h = (int)lround(dh);
      if (dst.w < 1) {
        dst.w = 1;
      }
      if (dst.h < 1) {
        dst.h = 1;
      }
      (void)SDL_SetTextureColorMod(texture, r, g, b);
      (void)SDL_SetTextureAlphaMod(texture, a);
      (void)SDL_RenderCopyEx(ctx->renderer, texture, &src, &dst, 0.0, NULL, flip);
      return;
    }
  }
#endif

  if (sprite_index >= 0 && ctx->game_data != NULL && (size_t)sprite_index < ctx->game_data->sprite_count) {
    sprite = &ctx->game_data->sprites[(size_t)sprite_index];
    (void)sprite;
  }
  {
    SDL_Rect rect = {0};
    int draw_w = (int)lround(fabs((double)width * xscale));
    int draw_h = (int)lround(fabs((double)height * yscale));
    if (draw_w < 1) {
      draw_w = 1;
    }
    if (draw_h < 1) {
      draw_h = 1;
    }
    rect.x = (int)lround(draw_x);
    rect.y = (int)lround(draw_y);
    rect.w = draw_w;
    rect.h = draw_h;
    (void)SDL_SetRenderDrawColor(ctx->renderer, r, g, b, a);
    (void)SDL_RenderFillRect(ctx->renderer, &rect);
  }
}

static void bs_sdl_draw_sprite(void *userdata,
                               const bs_game_runner *runner,
                               int32_t sprite_index,
                               int32_t image_index,
                               double x,
                               double y,
                               int32_t blend_color,
                               double alpha) {
  bs_sdl_draw_sprite_ext(userdata,
                         runner,
                         sprite_index,
                         image_index,
                         x,
                         y,
                         1.0,
                         1.0,
                         0.0,
                         blend_color,
                         alpha);
}

static void bs_sdl_draw_text(void *userdata,
                             const bs_game_runner *runner,
                             const char *text,
                             double x,
                             double y,
                             int32_t font_index,
                             int32_t color,
                             double xscale,
                             double yscale) {
  bs_sdl_draw_context *ctx = (bs_sdl_draw_context *)userdata;
  int line_height = 10;
  const bs_font_data *font = NULL;
  size_t line_count = 1;
  double text_height = 0.0;
  const char *line_start = NULL;
  const char *p = NULL;
  double start_y = 0.0;
  double screen_x = x;
  double screen_y = y;
  uint8_t r = 255;
  uint8_t g = 255;
  uint8_t b = 255;
  uint8_t a = 255;
  if (ctx == NULL || ctx->renderer == NULL || runner == NULL || text == NULL) {
    return;
  }

  if (ctx->game_data != NULL && font_index >= 0 && (size_t)font_index < ctx->game_data->font_count) {
    font = &ctx->game_data->fonts[(size_t)font_index];
    if (font->em_size > 0) {
      line_height = font->em_size;
    }
  }
  if (xscale <= 0.0) {
    xscale = 1.0;
  }
  if (yscale <= 0.0) {
    yscale = 1.0;
  }
  bs_sdl_world_to_screen(runner, x, y, &screen_x, &screen_y);
  line_height = (int)(line_height * yscale);
  if (line_height < 4) {
    line_height = 4;
  }
  for (p = text; *p != '\0'; p++) {
    if (*p == '\n') {
      line_count++;
    }
  }
  text_height = (double)line_count * (double)line_height;
  start_y = screen_y;
  if (runner->draw_valign == 1) {
    start_y = screen_y - (text_height / 2.0);
  } else if (runner->draw_valign == 2) {
    start_y = screen_y - text_height;
  }

  bs_sdl_unpack_color(color, (uint8_t)runner->draw_alpha, &r, &g, &b, &a);
  (void)SDL_SetRenderDrawColor(ctx->renderer, r, g, b, a);

  line_start = text;
  while (line_start != NULL && *line_start != '\0') {
    const char *line_end = strchr(line_start, '\n');
    size_t line_len = (line_end != NULL) ? (size_t)(line_end - line_start) : strlen(line_start);
    double line_width = bs_sdl_measure_line_width(font, line_start, line_len, xscale);
    double cursor_x = screen_x;
    double cursor_y = start_y;
    size_t at = 0;

    if (runner->draw_halign == 1) {
      cursor_x = screen_x - (line_width / 2.0);
    } else if (runner->draw_halign == 2) {
      cursor_x = screen_x - line_width;
    }

    while (at < line_len) {
      SDL_Rect glyph_rect = {0};
      uint32_t cp = 0;
      size_t consumed = bs_sdl_utf8_decode(line_start + at, line_len - at, &cp);
      const bs_font_glyph_data *glyph = NULL;
      int advance = (int)(6.0 * xscale);
      if (consumed == 0) {
        break;
      }
      at += consumed;
      if (cp == '\r') {
        continue;
      }
      if (advance < 2) {
        advance = 2;
      }

      glyph_rect.x = (int)cursor_x;
      glyph_rect.y = (int)cursor_y;
      glyph_rect.w = (int)(5.0 * xscale);
      glyph_rect.h = line_height;
      if (glyph_rect.w < 1) {
        glyph_rect.w = 1;
      }

      if (font != NULL) {
        glyph = bs_sdl_find_glyph_codepoint(font, cp);
        if (glyph != NULL) {
          int glyph_offset = (int)(int16_t)glyph->offset;
          int glyph_shift = (int)(int16_t)glyph->shift;
          int gw = (int)(glyph->width * xscale);
          int gh = (int)(glyph->height * yscale);
          glyph_rect.x = (int)(cursor_x + (glyph_offset * xscale));
          glyph_rect.w = gw > 0 ? gw : glyph_rect.w;
          glyph_rect.h = gh > 0 ? gh : glyph_rect.h;
          advance = (int)(glyph_shift * xscale);
          if (advance <= 0) {
            advance = glyph_rect.w + 1;
          }
        }
      }

#if defined(BS_HAVE_SDL_IMAGE)
      if (glyph != NULL &&
          ctx->texture_pages_ready &&
          font != NULL &&
          font->tpag_index >= 0 &&
          (size_t)font->tpag_index < ctx->game_data->texture_page_item_count) {
        const bs_texture_page_item_data *font_tpag = &ctx->game_data->texture_page_items[(size_t)font->tpag_index];
        if (font_tpag->texture_page_id < ctx->texture_page_count &&
            ctx->texture_pages[font_tpag->texture_page_id] != NULL) {
          SDL_Texture *texture = ctx->texture_pages[font_tpag->texture_page_id];
          SDL_Rect src = {0};
          src.x = (int)font_tpag->source_x + (int)glyph->x;
          src.y = (int)font_tpag->source_y + (int)glyph->y;
          src.w = (int)glyph->width;
          src.h = (int)glyph->height;
          if (src.w > 0 && src.h > 0) {
            (void)SDL_SetTextureColorMod(texture, r, g, b);
            (void)SDL_SetTextureAlphaMod(texture, a);
            (void)SDL_RenderCopy(ctx->renderer, texture, &src, &glyph_rect);
          }
        } else {
          (void)SDL_RenderFillRect(ctx->renderer, &glyph_rect);
        }
      } else {
        (void)SDL_RenderFillRect(ctx->renderer, &glyph_rect);
      }
#else
      (void)SDL_RenderFillRect(ctx->renderer, &glyph_rect);
#endif
      cursor_x += advance;
    }

    start_y += line_height;
    line_start = (line_end != NULL) ? (line_end + 1) : NULL;
  }
}

static void bs_sdl_draw_rect(void *userdata,
                             const bs_game_runner *runner,
                             double x1,
                             double y1,
                             double x2,
                             double y2,
                             bool outline,
                             int32_t color) {
  bs_sdl_draw_context *ctx = (bs_sdl_draw_context *)userdata;
  SDL_Rect rect = {0};
  uint8_t r = 255;
  uint8_t g = 255;
  uint8_t b = 255;
  uint8_t a = 255;
  double screen_x1 = x1;
  double screen_y1 = y1;
  double screen_x2 = x2;
  double screen_y2 = y2;
  
  if (ctx == NULL || ctx->renderer == NULL) {
    return;
  }
  
  /* Transform world coordinates to screen coordinates */
  bs_sdl_world_to_screen(runner, x1, y1, &screen_x1, &screen_y1);
  bs_sdl_world_to_screen(runner, x2, y2, &screen_x2, &screen_y2);
  
  rect.x = (int)(screen_x1 < screen_x2 ? screen_x1 : screen_x2);
  rect.y = (int)(screen_y1 < screen_y2 ? screen_y1 : screen_y2);
  rect.w = abs((int)screen_x2 - (int)screen_x1);
  rect.h = abs((int)screen_y2 - (int)screen_y1);
  if (rect.w < 1) {
    rect.w = 1;
  }
  if (rect.h < 1) {
    rect.h = 1;
  }

  bs_sdl_unpack_color(color, 255, &r, &g, &b, &a);
  (void)SDL_SetRenderDrawColor(ctx->renderer, r, g, b, a);
  if (outline) {
    (void)SDL_RenderDrawRect(ctx->renderer, &rect);
  } else {
    (void)SDL_RenderFillRect(ctx->renderer, &rect);
  }
}

static int32_t bs_sdl_key_to_vk(SDL_Keycode key) {
  switch (key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      return 13;
    case SDLK_ESCAPE:
      return 27;
    case SDLK_SPACE:
      return 32;
    case SDLK_BACKSPACE:
      return 8;
    case SDLK_TAB:
      return 9;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
      return 16;
    case SDLK_LCTRL:
    case SDLK_RCTRL:
      return 17;
    case SDLK_UP:
      return 38;
    case SDLK_DOWN:
      return 40;
    case SDLK_LEFT:
      return 37;
    case SDLK_RIGHT:
      return 39;
    case SDLK_F4:
      return 115;
    default:
      break;
  }

  if (key >= SDLK_a && key <= SDLK_z) {
    return (int32_t)(key - SDLK_a + 'A');
  }
  if (key >= SDLK_0 && key <= SDLK_9) {
    return (int32_t)key;
  }
  return -1;
}

static int32_t bs_sdl_scancode_to_vk(SDL_Scancode scancode) {
  switch (scancode) {
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
      return 13;
    case SDL_SCANCODE_ESCAPE:
      return 27;
    case SDL_SCANCODE_SPACE:
      return 32;
    case SDL_SCANCODE_BACKSPACE:
      return 8;
    case SDL_SCANCODE_TAB:
      return 9;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:
      return 16;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:
      return 17;
    case SDL_SCANCODE_UP:
      return 38;
    case SDL_SCANCODE_DOWN:
      return 40;
    case SDL_SCANCODE_LEFT:
      return 37;
    case SDL_SCANCODE_RIGHT:
      return 39;
    case SDL_SCANCODE_F4:
      return 115;
    default:
      break;
  }

  if (scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z) {
    return (int32_t)(scancode - SDL_SCANCODE_A + 'A');
  }
  if (scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9) {
    return (int32_t)(scancode - SDL_SCANCODE_1 + '1');
  }
  if (scancode == SDL_SCANCODE_0) {
    return '0';
  }
  return -1;
}

static uint32_t bs_target_frame_ms(const bs_game_runner *runner) {
  int32_t room_speed = 30;
  if (runner != NULL && runner->current_room != NULL && runner->current_room->speed > 0) {
    room_speed = runner->current_room->speed;
  }
  if (room_speed <= 0) {
    room_speed = 30;
  }
  return (uint32_t)(1000 / room_speed);
}

static void bs_sdl_pick_logical_size(const bs_game_runner *runner,
                                     int fallback_w,
                                     int fallback_h,
                                     int *out_w,
                                     int *out_h) {
  int width = fallback_w > 0 ? fallback_w : 640;
  int height = fallback_h > 0 ? fallback_h : 480;
  if (runner != NULL && runner->current_room != NULL) {
    const bs_room_data *room = runner->current_room;
    const bs_room_view_data *chosen_view = NULL;
    if (room->views != NULL && room->view_count > 0) {
      for (size_t i = 0; i < room->view_count; i++) {
        const bs_room_view_data *view = &room->views[i];
        if (view->enabled && view->view_w > 0 && view->view_h > 0) {
          chosen_view = view;
          break;
        }
      }
      if (chosen_view == NULL && room->views[0].view_w > 0 && room->views[0].view_h > 0) {
        chosen_view = &room->views[0];
      }
    }
    if (chosen_view != NULL) {
      width = chosen_view->view_w;
      height = chosen_view->view_h;
    } else if (room->width > 0 && room->height > 0) {
      width = room->width;
      height = room->height;
    }
  }
  if (out_w != NULL) {
    *out_w = width;
  }
  if (out_h != NULL) {
    *out_h = height;
  }
}

/* ── Audio backend (SDL_mixer) ──────────────────────────────────────────── */
#if defined(BS_HAVE_SDL_MIXER)

#define BS_AUDIO_MAX_CHANNELS 128
#define BS_AUDIO_HANDLE_BASE 1000

typedef struct bs_audio_channel_info {
  int32_t handle;       /* unique handle returned to GML */
  int32_t sound_index;  /* SOND index (-1 = unused) */
  Mix_Chunk *chunk;     /* loaded chunk (owned if from memory) */
  bool loop;
} bs_audio_channel_info;

typedef struct bs_sdl_audio_context {
  const bs_game_data *game_data;
  bs_audio_channel_info channels[BS_AUDIO_MAX_CHANNELS];
  int32_t next_handle;
  double master_gain;   /* 0.0–1.0 */

  /* Lazy-loaded chunk cache: one Mix_Chunk* per AUDO entry. */
  Mix_Chunk **chunk_cache;
  size_t chunk_cache_count;

  /* Lazy-loaded chunk cache for external files (indexed by SOND index). */
  Mix_Chunk **ext_chunk_cache;
  size_t ext_chunk_cache_count;

  /* Base directory of the game file (for loading external OGGs). */
  char game_dir[1024];
} bs_sdl_audio_context;

static bs_sdl_audio_context g_audio_ctx;

static bool bs_sdl_audio_init(const bs_game_data *game_data) {
  memset(&g_audio_ctx, 0, sizeof(g_audio_ctx));
  g_audio_ctx.game_data = game_data;
  g_audio_ctx.next_handle = BS_AUDIO_HANDLE_BASE;
  g_audio_ctx.master_gain = 1.0;

  /* Extract directory from game_path for external audio file loading. */
  {
    const char *slash = NULL;
    const char *p = game_data->game_path;
    while (*p != '\0') {
      if (*p == '/' || *p == '\\') {
        slash = p;
      }
      p++;
    }
    if (slash != NULL) {
      size_t len = (size_t)(slash - game_data->game_path);
      if (len >= sizeof(g_audio_ctx.game_dir)) {
        len = sizeof(g_audio_ctx.game_dir) - 1;
      }
      memcpy(g_audio_ctx.game_dir, game_data->game_path, len);
      g_audio_ctx.game_dir[len] = '\0';
    } else {
      g_audio_ctx.game_dir[0] = '.';
      g_audio_ctx.game_dir[1] = '\0';
    }
  }

  if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
    fprintf(stderr, "Mix_OpenAudio failed: %s\n", Mix_GetError());
    return false;
  }
  Mix_AllocateChannels(BS_AUDIO_MAX_CHANNELS);
  for (int i = 0; i < BS_AUDIO_MAX_CHANNELS; i++) {
    g_audio_ctx.channels[i].sound_index = -1;
    g_audio_ctx.channels[i].handle = -1;
    g_audio_ctx.channels[i].chunk = NULL;
    g_audio_ctx.channels[i].loop = false;
  }

  if (game_data->audio_data_count > 0) {
    g_audio_ctx.chunk_cache = (Mix_Chunk **)calloc(game_data->audio_data_count, sizeof(Mix_Chunk *));
    g_audio_ctx.chunk_cache_count = game_data->audio_data_count;
  }

  if (game_data->sound_count > 0) {
    g_audio_ctx.ext_chunk_cache = (Mix_Chunk **)calloc(game_data->sound_count, sizeof(Mix_Chunk *));
    g_audio_ctx.ext_chunk_cache_count = game_data->sound_count;
  }

  printf("Audio: SDL_mixer initialized (%zu sounds, %zu audio entries, game_dir='%s')\n",
         game_data->sound_count, game_data->audio_data_count, g_audio_ctx.game_dir);
  return true;
}

static void bs_sdl_audio_quit(void) {
  for (int i = 0; i < BS_AUDIO_MAX_CHANNELS; i++) {
    g_audio_ctx.channels[i].sound_index = -1;
    g_audio_ctx.channels[i].chunk = NULL;
  }
  if (g_audio_ctx.chunk_cache != NULL) {
    for (size_t i = 0; i < g_audio_ctx.chunk_cache_count; i++) {
      if (g_audio_ctx.chunk_cache[i] != NULL) {
        Mix_FreeChunk(g_audio_ctx.chunk_cache[i]);
      }
    }
    free(g_audio_ctx.chunk_cache);
    g_audio_ctx.chunk_cache = NULL;
  }
  if (g_audio_ctx.ext_chunk_cache != NULL) {
    for (size_t i = 0; i < g_audio_ctx.ext_chunk_cache_count; i++) {
      if (g_audio_ctx.ext_chunk_cache[i] != NULL) {
        Mix_FreeChunk(g_audio_ctx.ext_chunk_cache[i]);
      }
    }
    free(g_audio_ctx.ext_chunk_cache);
    g_audio_ctx.ext_chunk_cache = NULL;
  }
  Mix_CloseAudio();
}

static Mix_Chunk *bs_sdl_audio_get_chunk(int32_t audio_id) {
  if (audio_id < 0 || (size_t)audio_id >= g_audio_ctx.chunk_cache_count) {
    return NULL;
  }
  if (g_audio_ctx.chunk_cache[audio_id] != NULL) {
    return g_audio_ctx.chunk_cache[audio_id];
  }

  const bs_game_data *gd = g_audio_ctx.game_data;
  const bs_audio_data *ad = &gd->audio_data[audio_id];
  if (ad->length == 0) {
    return NULL;
  }

  const uint8_t *raw = gd->file_data + ad->data_offset;
  SDL_RWops *rw = SDL_RWFromConstMem(raw, (int)ad->length);
  if (rw == NULL) {
    return NULL;
  }

  Mix_Chunk *chunk = Mix_LoadWAV_RW(rw, 1); /* freesrc=1 */
  if (chunk == NULL) {
    return NULL;
  }
  g_audio_ctx.chunk_cache[audio_id] = chunk;
  return chunk;
}

static Mix_Chunk *bs_sdl_audio_get_ext_chunk(int32_t sound_index) {
  if (sound_index < 0 || (size_t)sound_index >= g_audio_ctx.ext_chunk_cache_count) {
    return NULL;
  }
  if (g_audio_ctx.ext_chunk_cache[sound_index] != NULL) {
    return g_audio_ctx.ext_chunk_cache[sound_index];
  }

  const bs_game_data *gd = g_audio_ctx.game_data;
  const bs_sound_data *snd = &gd->sounds[sound_index];
  char path[2048];

  /* Check if file_name already includes the extension. */
  const char *ext = snd->extension;
  size_t fn_len = strlen(snd->file_name);
  size_t ext_len = ext != NULL ? strlen(ext) : 0;
  bool has_ext = (ext_len > 0 && fn_len >= ext_len &&
                  strcmp(snd->file_name + fn_len - ext_len, ext) == 0);
  const char *suffix = has_ext ? "" : (ext != NULL ? ext : "");

  /* Try game_dir/<file_name>[<extension>] first. */
  snprintf(path, sizeof(path), "%s/%s%s",
           g_audio_ctx.game_dir, snd->file_name, suffix);
  Mix_Chunk *chunk = Mix_LoadWAV(path);

  /* Fallback: game_dir/music/<file_name>[<extension>]. */
  if (chunk == NULL) {
    snprintf(path, sizeof(path), "%s/music/%s%s",
             g_audio_ctx.game_dir, snd->file_name, suffix);
    chunk = Mix_LoadWAV(path);
  }

  if (chunk == NULL) {
    fprintf(stderr, "Audio: failed to load external '%s%s': %s\n",
            snd->file_name, suffix, Mix_GetError());
    return NULL;
  }

  g_audio_ctx.ext_chunk_cache[sound_index] = chunk;
  return chunk;
}

static int32_t bs_sdl_audio_play(void *userdata,
                                  const struct bs_game_runner *runner,
                                  int32_t sound_index,
                                  bool loop,
                                  double priority) {
  (void)priority;
  (void)userdata;
  const bs_game_data *gd = runner->game_data;
  if (gd == NULL || sound_index < 0 || (size_t)sound_index >= gd->sound_count) {
    return -1;
  }
  const bs_sound_data *snd = &gd->sounds[sound_index];
  int32_t audio_id = snd->audio_id;

  Mix_Chunk *chunk = NULL;
  bool is_ogg = snd->extension != NULL &&
                (strcmp(snd->extension, ".ogg") == 0 ||
                 strcmp(snd->extension, ".OGG") == 0);

  if (is_ogg) {
    /* OGG sounds: prefer external file (the embedded AUDO entry is often
       just a tiny WAV placeholder, not the real music track). */
    chunk = bs_sdl_audio_get_ext_chunk(sound_index);
  }
  if (chunk == NULL && audio_id >= 0) {
    /* Embedded audio (WAV SFX, or fallback if external not found). */
    chunk = bs_sdl_audio_get_chunk(audio_id);
  }
  if (chunk == NULL) {
    return -1;
  }

  /* Apply per-sound volume (0.0–1.0 from SOND data) scaled by master gain. */
  int vol = (int)(snd->volume * g_audio_ctx.master_gain * 128.0);
  if (vol > 128) { vol = 128; }
  if (vol < 0) { vol = 0; }
  Mix_VolumeChunk(chunk, vol);

  int loops = loop ? -1 : 0;
  int channel = Mix_PlayChannel(-1, chunk, loops);
  if (channel < 0) {
    return -1;
  }

  int32_t handle = g_audio_ctx.next_handle++;
  if (channel < BS_AUDIO_MAX_CHANNELS) {
    g_audio_ctx.channels[channel].handle = handle;
    g_audio_ctx.channels[channel].sound_index = sound_index;
    g_audio_ctx.channels[channel].chunk = chunk;
    g_audio_ctx.channels[channel].loop = loop;
  }
  return handle;
}

/* Find the mixer channel for a given handle, or -1 if not found. */
static int bs_sdl_audio_find_channel_by_handle(int32_t handle) {
  for (int i = 0; i < BS_AUDIO_MAX_CHANNELS; i++) {
    if (g_audio_ctx.channels[i].handle == handle && Mix_Playing(i)) {
      return i;
    }
  }
  return -1;
}

/* A handle_or_index value >= BS_AUDIO_HANDLE_BASE is a handle; otherwise it's
   a sound index. When it's a sound index, operate on ALL channels playing that
   sound. Return value: first matching channel, or -1. */
static void bs_sdl_audio_stop(void *userdata, int32_t handle_or_index) {
  (void)userdata;
  if (handle_or_index >= BS_AUDIO_HANDLE_BASE) {
    int ch = bs_sdl_audio_find_channel_by_handle(handle_or_index);
    if (ch >= 0) {
      Mix_HaltChannel(ch);
      g_audio_ctx.channels[ch].sound_index = -1;
      g_audio_ctx.channels[ch].handle = -1;
    }
  } else {
    /* sound_index: stop all channels playing this sound */
    for (int i = 0; i < BS_AUDIO_MAX_CHANNELS; i++) {
      if (g_audio_ctx.channels[i].sound_index == handle_or_index && Mix_Playing(i)) {
        Mix_HaltChannel(i);
        g_audio_ctx.channels[i].sound_index = -1;
        g_audio_ctx.channels[i].handle = -1;
      }
    }
  }
}

static void bs_sdl_audio_stop_all(void *userdata) {
  (void)userdata;
  Mix_HaltChannel(-1);
  for (int i = 0; i < BS_AUDIO_MAX_CHANNELS; i++) {
    g_audio_ctx.channels[i].sound_index = -1;
    g_audio_ctx.channels[i].handle = -1;
  }
}

static void bs_sdl_audio_set_gain(void *userdata, int32_t handle, double volume, double duration_ms) {
  (void)userdata;
  (void)duration_ms; /* fade not implemented yet */
  int vol = (int)(volume * g_audio_ctx.master_gain * 128.0);
  if (vol > 128) { vol = 128; }
  if (vol < 0) { vol = 0; }

  if (handle >= BS_AUDIO_HANDLE_BASE) {
    int ch = bs_sdl_audio_find_channel_by_handle(handle);
    if (ch >= 0) {
      Mix_Volume(ch, vol);
    }
  } else {
    for (int i = 0; i < BS_AUDIO_MAX_CHANNELS; i++) {
      if (g_audio_ctx.channels[i].sound_index == handle && Mix_Playing(i)) {
        Mix_Volume(i, vol);
      }
    }
  }
}

static void bs_sdl_audio_set_pitch(void *userdata, int32_t handle, double pitch) {
  (void)userdata;
  (void)handle;
  (void)pitch;
  /* SDL_mixer doesn't support pitch shifting natively. Silently ignore. */
}

static bool bs_sdl_audio_is_playing(void *userdata, int32_t handle_or_index) {
  (void)userdata;
  if (handle_or_index >= BS_AUDIO_HANDLE_BASE) {
    int ch = bs_sdl_audio_find_channel_by_handle(handle_or_index);
    return ch >= 0;
  } else {
    for (int i = 0; i < BS_AUDIO_MAX_CHANNELS; i++) {
      if (g_audio_ctx.channels[i].sound_index == handle_or_index && Mix_Playing(i)) {
        return true;
      }
    }
    return false;
  }
}

static void bs_sdl_audio_pause(void *userdata, int32_t handle_or_index) {
  (void)userdata;
  if (handle_or_index >= BS_AUDIO_HANDLE_BASE) {
    int ch = bs_sdl_audio_find_channel_by_handle(handle_or_index);
    if (ch >= 0) {
      Mix_Pause(ch);
    }
  } else {
    for (int i = 0; i < BS_AUDIO_MAX_CHANNELS; i++) {
      if (g_audio_ctx.channels[i].sound_index == handle_or_index && Mix_Playing(i)) {
        Mix_Pause(i);
      }
    }
  }
}

static void bs_sdl_audio_resume(void *userdata, int32_t handle_or_index) {
  (void)userdata;
  if (handle_or_index >= BS_AUDIO_HANDLE_BASE) {
    int ch = bs_sdl_audio_find_channel_by_handle(handle_or_index);
    if (ch >= 0) {
      Mix_Resume(ch);
    }
  } else {
    for (int i = 0; i < BS_AUDIO_MAX_CHANNELS; i++) {
      if (g_audio_ctx.channels[i].sound_index == handle_or_index) {
        Mix_Resume(i);
      }
    }
  }
}

static void bs_sdl_audio_set_master_gain(void *userdata, double volume) {
  (void)userdata;
  if (volume < 0.0) { volume = 0.0; }
  if (volume > 1.0) { volume = 1.0; }
  g_audio_ctx.master_gain = volume;
  int vol = (int)(volume * 128.0);
  Mix_MasterVolume(vol);
}

static void bs_sdl_audio_set_track_position(void *userdata, int32_t handle, double position) {
  (void)userdata;
  (void)handle;
  (void)position;
  /* Not easily supported with Mix_Chunk channels. Silently ignore. */
}

static double bs_sdl_audio_get_track_position(void *userdata, int32_t handle) {
  (void)userdata;
  (void)handle;
  return 0.0;
}

/* Called once after the audio channel finishes to clean up tracking. */
static void bs_sdl_audio_channel_finished(int channel) {
  if (channel >= 0 && channel < BS_AUDIO_MAX_CHANNELS) {
    g_audio_ctx.channels[channel].sound_index = -1;
    g_audio_ctx.channels[channel].handle = -1;
    g_audio_ctx.channels[channel].chunk = NULL;
    g_audio_ctx.channels[channel].loop = false;
  }
}

#endif /* BS_HAVE_SDL_MIXER */

int bs_run_sdl(const char *game_path) {
  bs_game_data game_data = {0};
  bs_vm vm = {0};
  bs_game_runner runner = {0};
  bs_sdl_draw_context draw_ctx = {0};
  SDL_Window *window = NULL;
  SDL_Renderer *renderer = NULL;
  int renderer_output_width = 0;
  int renderer_output_height = 0;
  int logical_width = 640;
  int logical_height = 480;
  bool has_vsync = false;
  bool running = true;
  bool trace_input = false;
  int max_frames = -1;
  int auto_key_frame = -1;
  int auto_key_code = 90;
  bool auto_key_hold = false;
  int window_width = 640;
  int window_height = 480;
  int status = 0;

  if (game_path == NULL) {
    game_path = "undertale/game.unx";
  }
  {
    const char *max_frames_env = getenv("BS_MAX_FRAMES");
    const char *auto_key_frame_env = getenv("BS_AUTOKEY_FRAME");
    const char *auto_key_code_env = getenv("BS_AUTOKEY_CODE");
    const char *auto_key_hold_env = getenv("BS_AUTOKEY_HOLD");
    const char *trace_input_env = getenv("BS_TRACE_INPUT");
    if (max_frames_env != NULL) {
      max_frames = atoi(max_frames_env);
    }
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
    if (trace_input_env != NULL &&
        (strcmp(trace_input_env, "1") == 0 || strcmp(trace_input_env, "true") == 0)) {
      trace_input = true;
    }
  }

  if (!bs_form_reader_read(game_path, &game_data)) {
    fprintf(stderr, "Failed to read game data: %s\n", game_path);
    return 1;
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    status = 1;
    goto cleanup;
  }
#if defined(BS_HAVE_SDL_IMAGE)
  if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0) {
    fprintf(stderr, "IMG_Init PNG failed: %s\n", IMG_GetError());
  }
#endif

  if (game_data.gen8.window_width > 0) {
    window_width = (int)game_data.gen8.window_width;
  }
  if (game_data.gen8.window_height > 0) {
    window_height = (int)game_data.gen8.window_height;
  }

  window = SDL_CreateWindow(game_data.gen8.display_name != NULL ? game_data.gen8.display_name : "Butterscotch-C",
                            SDL_WINDOWPOS_CENTERED,
                            SDL_WINDOWPOS_CENTERED,
                            window_width,
                            window_height,
                            SDL_WINDOW_SHOWN);
  if (window == NULL) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    status = 1;
    goto cleanup;
  }

  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (renderer != NULL) {
    has_vsync = true;
  } else {
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
  }
  if (renderer == NULL) {
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  }
  if (renderer == NULL) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    status = 1;
    goto cleanup;
  }
  (void)SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  (void)SDL_RenderSetIntegerScale(renderer, SDL_FALSE);
  if (SDL_GetRendererOutputSize(renderer, &renderer_output_width, &renderer_output_height) != 0) {
    renderer_output_width = window_width;
    renderer_output_height = window_height;
  }
  (void)SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

  printf("Butterscotch-C SDL frontend\n");
  printf("Game file: %s\n", game_data.game_path);
  printf("Window: %dx%d\n", window_width, window_height);
  printf("Renderer output: %dx%d (vsync=%s)\n",
         renderer_output_width,
         renderer_output_height,
         has_vsync ? "on" : "off");

  bs_vm_init(&vm, &game_data);
  bs_register_builtins(&vm);
  bs_game_runner_init(&runner, &game_data, &vm);
  runner.surface_width = window_width;
  runner.surface_height = window_height;
  bs_sdl_pick_logical_size(&runner, window_width, window_height, &logical_width, &logical_height);
  if (SDL_RenderSetLogicalSize(renderer, logical_width, logical_height) != 0) {
    fprintf(stderr, "SDL_RenderSetLogicalSize failed: %s\n", SDL_GetError());
  }
  printf("Logical size: %dx%d\n", logical_width, logical_height);

  draw_ctx.renderer = renderer;
  draw_ctx.game_data = &game_data;
  draw_ctx.texture_pages = NULL;
  draw_ctx.texture_page_count = 0;
  draw_ctx.texture_pages_ready = false;
  if (bs_sdl_load_texture_pages(&draw_ctx, &game_data)) {
    printf("Loaded %zu texture pages for sprite rendering\n", draw_ctx.texture_page_count);
  } else {
    printf("Texture pages unavailable: using placeholder sprite/text rendering\n");
  }
  runner.render.userdata = &draw_ctx;
  runner.render.clear = bs_sdl_clear;
  runner.render.draw_sprite = bs_sdl_draw_sprite;
  runner.render.draw_sprite_ext = bs_sdl_draw_sprite_ext;
  runner.render.draw_sprite_part_ext = bs_sdl_draw_sprite_part_ext;
  runner.render.draw_background = bs_sdl_draw_background;
  runner.render.draw_tile = bs_sdl_draw_tile;
  runner.render.draw_text = bs_sdl_draw_text;
  runner.render.draw_rect = bs_sdl_draw_rect;

#if defined(BS_HAVE_SDL_MIXER)
  if (bs_sdl_audio_init(&game_data)) {
    Mix_ChannelFinished(bs_sdl_audio_channel_finished);
    runner.audio.userdata = &g_audio_ctx;
    runner.audio.play_sound = bs_sdl_audio_play;
    runner.audio.stop_sound = bs_sdl_audio_stop;
    runner.audio.stop_all = bs_sdl_audio_stop_all;
    runner.audio.set_gain = bs_sdl_audio_set_gain;
    runner.audio.set_pitch = bs_sdl_audio_set_pitch;
    runner.audio.is_playing = bs_sdl_audio_is_playing;
    runner.audio.pause_sound = bs_sdl_audio_pause;
    runner.audio.resume_sound = bs_sdl_audio_resume;
    runner.audio.set_master_gain = bs_sdl_audio_set_master_gain;
    runner.audio.set_track_position = bs_sdl_audio_set_track_position;
    runner.audio.get_track_position = bs_sdl_audio_get_track_position;
  }
#endif

  while (running && !runner.should_quit) {
    uint64_t frame_start_ticks = SDL_GetPerformanceCounter();
    uint64_t frame_freq = SDL_GetPerformanceFrequency();
    SDL_Event event;

    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        running = false;
        break;
      }
      if (event.type == SDL_KEYDOWN) {
        int32_t vk_code = 0;
        if (event.key.repeat != 0) {
          continue;
        }
        vk_code = bs_sdl_key_to_vk(event.key.keysym.sym);
        if (vk_code < 0) {
          vk_code = bs_sdl_scancode_to_vk(event.key.keysym.scancode);
        }
        if (vk_code >= 0) {
          if (trace_input) {
            printf("[SDL INPUT] down sym=%d sc=%d vk=%d\n",
                   (int)event.key.keysym.sym,
                   (int)event.key.keysym.scancode,
                   (int)vk_code);
          }
          bs_game_runner_on_key_down(&runner, vk_code);
        }
      } else if (event.type == SDL_KEYUP) {
        int32_t vk_code = bs_sdl_key_to_vk(event.key.keysym.sym);
        if (vk_code < 0) {
          vk_code = bs_sdl_scancode_to_vk(event.key.keysym.scancode);
        }
        if (vk_code >= 0) {
          if (trace_input) {
            printf("[SDL INPUT] up sym=%d sc=%d vk=%d\n",
                   (int)event.key.keysym.sym,
                   (int)event.key.keysym.scancode,
                   (int)vk_code);
          }
          bs_game_runner_on_key_up(&runner, vk_code);
        }
      }
    }

    if (!running) {
      break;
    }

    if (auto_key_frame >= 0 && (int)runner.frame_count == auto_key_frame) {
      bs_game_runner_on_key_down(&runner, auto_key_code);
    }
    if (!auto_key_hold && auto_key_frame >= 0 && (int)runner.frame_count == auto_key_frame + 1) {
      bs_game_runner_on_key_up(&runner, auto_key_code);
    }

    {
      int wanted_logical_width = logical_width;
      int wanted_logical_height = logical_height;
      bs_sdl_pick_logical_size(&runner,
                               window_width,
                               window_height,
                               &wanted_logical_width,
                               &wanted_logical_height);
      if (wanted_logical_width != logical_width || wanted_logical_height != logical_height) {
        if (SDL_RenderSetLogicalSize(renderer, wanted_logical_width, wanted_logical_height) != 0) {
          fprintf(stderr, "SDL_RenderSetLogicalSize failed: %s\n", SDL_GetError());
        } else {
          logical_width = wanted_logical_width;
          logical_height = wanted_logical_height;
          printf("Logical size updated: %dx%d (room=%d)\n",
                 logical_width,
                 logical_height,
                 runner.current_room_index);
          
          /* Debug: show view info for battle rooms */
          if (runner.current_room != NULL && runner.current_room->name != NULL &&
              (strstr(runner.current_room->name, "battle") != NULL || 
               strstr(runner.current_room->name, "floweybattle") != NULL)) {
            const bs_room_view_data *view = bs_sdl_choose_enabled_view(&runner);
            printf("[DEBUG] Battle room=%s size=%dx%d\n", 
                   runner.current_room->name, runner.current_room->width, runner.current_room->height);
            if (view != NULL) {
              printf("[DEBUG] View: enabled=%d view(%d,%d,%d,%d) port(%d,%d,%d,%d)\n",
                     view->enabled, view->view_x, view->view_y, view->view_w, view->view_h,
                     view->port_x, view->port_y, view->port_w, view->port_h);
            } else {
              printf("[DEBUG] No enabled view found\n");
            }
          }
        }
      }
    }

    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    bs_game_runner_step(&runner);
    if (runner.should_quit) {
      running = false;
    }
    if (max_frames > 0 && (int)runner.frame_count >= max_frames) {
      running = false;
    }


    SDL_RenderPresent(renderer);

    {
      uint64_t frame_end_ticks = SDL_GetPerformanceCounter();
      uint64_t elapsed_ms = 0;
      uint32_t target_ms = bs_target_frame_ms(&runner);
      if (frame_freq > 0) {
        elapsed_ms = (uint64_t)(((frame_end_ticks - frame_start_ticks) * 1000u) / frame_freq);
      }
      if (elapsed_ms < (uint64_t)target_ms) {
        SDL_Delay((uint32_t)((uint64_t)target_ms - elapsed_ms));
      }
    }
  }

cleanup:
  bs_game_runner_dispose(&runner);
  bs_vm_dispose(&vm);
  bs_game_data_free(&game_data);
  bs_sdl_free_texture_pages(&draw_ctx);

#if defined(BS_HAVE_SDL_MIXER)
  bs_sdl_audio_quit();
#endif

  if (renderer != NULL) {
    SDL_DestroyRenderer(renderer);
  }
  if (window != NULL) {
    SDL_DestroyWindow(window);
  }
#if defined(BS_HAVE_SDL_IMAGE)
  IMG_Quit();
#endif
  SDL_Quit();
  return status;
}
