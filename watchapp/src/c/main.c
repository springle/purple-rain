/* Purple Rain — a silent 0-2 h nowcast face for Pebble Time 2 (emery).
 *
 * Solid pixels: what the MRMS radar sees right now.
 * Dithered pixels: wherever HRRR puts rain in the next two hours.
 * White square-tailed vectors: ProbSevere motion on cells the radar sees.
 * Color ramps blue -> purple with intensity. No labels, no fallbacks.
 */
#include <pebble.h>
#include "masks.h"
#include "font5x7.h"

#define SCREEN_W 200
#define SCREEN_H 228
#define MAP_Y 25
#define GRID_W 25
#define GRID_H 21
#define NCELLS (GRID_W * GRID_H)
#define MAX_VECS 6

static Window *s_win;
static Layer *s_layer;

static uint8_t s_cells[NCELLS];        /* bits0-1 now sev, bits2-3 next-2h sev */
static uint8_t s_vecs[MAX_VECS * 4];   /* x, y(map), dx+64, dy+64 */
static int s_nvec = 0;
static int s_temp = -999, s_dew = -999, s_uv = -999, s_aqi = -999;
static char s_wind[16] = "";
static int s_health = 2;               /* red until first payload */
static bool s_have = false;

static uint8_t *s_rows[SCREEN_H];
static uint8_t c_black, c_white, c_gray, c_dim, c_rain, c_hvy, c_sev;
static uint8_t c_hgrn, c_hyel, c_hred;

static void px(int x, int y, uint8_t c) {
  if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H && s_rows[y]) s_rows[y][x] = c;
}

static void fill(int x, int y, int w, int h, uint8_t c) {
  for (int j = y; j < y + h; j++)
    for (int i = x; i < x + w; i++) px(i, j, c);
}

static void draw_line(int x0, int y0, int x1, int y1, uint8_t c) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
  int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
  int e = dx + dy;
  for (;;) {
    px(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * e;
    if (e2 >= dy) { e += dy; x0 += sx; }
    if (e2 <= dx) { e += dx; y0 += sy; }
  }
}

static int draw_text(const char *s, int x, int y, uint8_t c, int scale) {
  for (; *s; s++) {
    int idx = *s - 32;
    if (idx >= 0 && idx < 95) {
      const uint8_t *rows = FONT5X7[idx];
      for (int r = 0; r < 7; r++)
        for (int b = 0; b < 5; b++)
          if (rows[r] & (16 >> b)) fill(x + b * scale, y + r * scale, scale, scale, c);
    }
    x += 6 * scale;
  }
  return x;
}

/* severity palette: 0 none, 1 rain (blue), 2 heavy (deep blue), 3 severe (purple) */
static uint8_t sev_color(int sev) {
  return sev >= 3 ? c_sev : sev == 2 ? c_hvy : c_rain;
}

static void draw_map(void) {
  for (int my = 0; my < MASK_H; my++) {
    int sy = MAP_Y + my;
    int cj = my >> 3;
    for (int x = 0; x < MASK_W; x++) {
      /* base cartography */
      if (mask_bit(COAST_MASK, x, my)) px(x, sy, c_gray);
      else if (mask_bit(LAND_MASK, x, my) && !(x & 1) && !(sy & 1)) px(x, sy, c_dim);
      /* weather field: solid = now, 50% checkerboard = next 2 h */
      if (s_have) {
        int b = s_cells[cj * GRID_W + (x >> 3)];
        int now = b & 3, fut = (b >> 2) & 3;
        if (now) px(x, sy, sev_color(now));
        else if (fut && !((x + sy) & 1)) px(x, sy, sev_color(fut));
      }
    }
  }
  /* ProbSevere motion: thick square base at the cell, bare shaft, no head */
  for (int v = 0; v < s_nvec; v++) {
    int x = s_vecs[v * 4], y = MAP_Y + s_vecs[v * 4 + 1];
    int dx = (int)s_vecs[v * 4 + 2] - 64, dy = (int)s_vecs[v * 4 + 3] - 64;
    fill(x - 2, y - 2, 5, 5, c_white);
    draw_line(x, y, x + dx, y + dy, c_white);
  }
}

static void update_proc(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, GRect(0, 0, SCREEN_W, SCREEN_H), 0, GCornerNone);
  GBitmap *fb = graphics_capture_frame_buffer(ctx);
  if (!fb) return;
  for (int y = 0; y < SCREEN_H; y++) {
    GBitmapDataRowInfo ri = gbitmap_get_data_row_info(fb, y);
    s_rows[y] = ri.data;
  }

  /* header: clock + date cluster, health dot top-right */
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  char clk[8], date[16];
  if (clock_is_24h_style()) strftime(clk, sizeof clk, "%H:%M", t);
  else {
    strftime(clk, sizeof clk, "%I:%M", t);
    if (clk[0] == '0') memmove(clk, clk + 1, strlen(clk));
  }
  strftime(date, sizeof date, "%a %b %d", t);
  for (char *p = date; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
  int xa = draw_text(clk, 4, 4, c_white, 2);
  draw_text(date, xa + 10, 4, c_gray, 2);
  uint8_t dot = s_health == 0 ? c_hgrn : s_health == 1 ? c_hyel : c_hred;
  fill(190, 7, 5, 5, dot);
  fill(0, 24, SCREEN_W, 1, c_dim);

  draw_map();

  fill(0, 193, SCREEN_W, 1, c_dim);
  /* rail: all current obs, gray (temp white for hierarchy) */
  char buf[20];
  if (s_temp > -999) {
    snprintf(buf, sizeof buf, "%d~", s_temp);
    draw_text(buf, 4, 203, c_white, 2);
  }
  if (s_dew > -999) {
    snprintf(buf, sizeof buf, "DEW %d~", s_dew);
    draw_text(buf, 50, 199, c_gray, 1);
  }
  if (s_uv > -999) {
    snprintf(buf, sizeof buf, "UV %d", s_uv);
    draw_text(buf, 50, 212, c_gray, 1);
  }
  if (s_aqi > -999) {
    snprintf(buf, sizeof buf, "AQI %d", s_aqi);
    draw_text(buf, 124, 199, c_gray, 1);
  }
  if (s_wind[0]) draw_text(s_wind, 124, 212, c_gray, 1);

  graphics_release_frame_buffer(ctx, fb);
}

static void inbox(DictionaryIterator *it, void *ctx) {
  Tuple *tp;
  if ((tp = dict_find(it, MESSAGE_KEY_CELLS)) && tp->length == NCELLS)
    memcpy(s_cells, tp->value->data, NCELLS);
  if ((tp = dict_find(it, MESSAGE_KEY_NVEC))) s_nvec = tp->value->int32;
  if (s_nvec > MAX_VECS) s_nvec = MAX_VECS;
  if ((tp = dict_find(it, MESSAGE_KEY_VECS)) && tp->length >= (uint16_t)(s_nvec * 4))
    memcpy(s_vecs, tp->value->data, s_nvec * 4);
  if ((tp = dict_find(it, MESSAGE_KEY_TEMP))) s_temp = tp->value->int32;
  if ((tp = dict_find(it, MESSAGE_KEY_DEW))) s_dew = tp->value->int32;
  if ((tp = dict_find(it, MESSAGE_KEY_UV))) s_uv = tp->value->int32;
  if ((tp = dict_find(it, MESSAGE_KEY_AQI))) s_aqi = tp->value->int32;
  if ((tp = dict_find(it, MESSAGE_KEY_HEALTH))) s_health = tp->value->int32;
  if ((tp = dict_find(it, MESSAGE_KEY_WIND)))
    strncpy(s_wind, tp->value->cstring, sizeof s_wind - 1);
  s_have = true;
  layer_mark_dirty(s_layer);
}

static void tick(struct tm *t, TimeUnits u) { layer_mark_dirty(s_layer); }

static void win_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, update_proc);
  layer_add_child(root, s_layer);
}

static void win_unload(Window *w) { layer_destroy(s_layer); }

static void init(void) {
  c_black = GColorBlack.argb;
  c_white = GColorWhite.argb;
  c_gray = GColorFromHEX(0xAAAAAA).argb;
  c_dim = GColorFromHEX(0x555555).argb;
  c_rain = GColorFromHEX(0x00AAFF).argb;   /* rain: sky blue */
  c_hvy = GColorFromHEX(0x0000FF).argb;    /* heavy: deep blue */
  c_sev = GColorFromHEX(0xAA00FF).argb;    /* severe: purple rain */
  c_hgrn = GColorFromHEX(0x00FF00).argb;
  c_hyel = GColorFromHEX(0xFFFF00).argb;
  c_hred = GColorFromHEX(0xFF0000).argb;

  s_win = window_create();
  window_set_window_handlers(s_win, (WindowHandlers){ .load = win_load, .unload = win_unload });
  window_stack_push(s_win, true);
  tick_timer_service_subscribe(MINUTE_UNIT, tick);
  app_message_register_inbox_received(inbox);
  app_message_open(2048, 64);
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  window_destroy(s_win);
}

int main(void) { init(); app_event_loop(); deinit(); }
