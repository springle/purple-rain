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

static uint8_t s_cells[NCELLS];        /* low nibble: now level, high: next-2h (0-15 log intensity) */
static uint8_t s_now[NCELLS], s_fut[NCELLS];
/* thresholds in level space, level = 2*log2(mm/h + 1) */
#define L_FRINGE 1.2f
#define L_RAIN 2.36f   /* 0.05 in/h */
#define L_HVY 9.44f    /* 1 in/h */
#define L_SEV 11.39f   /* 2 in/h */
static uint8_t s_vecs[MAX_VECS * 4];   /* x, y(map), dx+64, dy+64 */
static int s_nvec = 0;
static int s_temp = -999, s_dew = -999, s_uv = -999, s_aqi = -999;
static char s_wind[16] = "";
static int s_health = 2;               /* red until first payload */
static bool s_have = false;

/* persist keys: watchfaces are killed on every trip into the system UI, so
 * the last payload must survive relaunch (cells chunked under the 256 B cap) */
#define PKEY_CELLS0 10
#define PKEY_CELLS1 11
#define PKEY_CELLS2 12
#define PKEY_VECS 13
#define PKEY_META 14
typedef struct __attribute__((packed)) {
  int16_t temp, dew, uv, aqi;
  uint8_t health, nvec;
  char wind[16];
} Meta;

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

static uint8_t level_color(float v) {
  return v >= L_SEV ? c_sev : v >= L_HVY ? c_hvy : c_rain;
}

static void decode_cells(void) {
  for (int k = 0; k < NCELLS; k++) {
    s_now[k] = s_cells[k] & 15;
    s_fut[k] = s_cells[k] >> 4;
  }
}

/* bilinear sample of a 25x21 level grid at a map pixel: cell centers sit at
 * (i*8+4, j*8+4), so intensity glides between cells instead of stepping in
 * 8-px blocks — the difference between radar and minecraft */
static float bilin(const uint8_t *g, int x, int y) {
  float gx = (x - 4) / 8.0f, gy = (y - 4) / 8.0f;
  int i0 = gx < 0 ? 0 : (int)gx;
  int j0 = gy < 0 ? 0 : (int)gy;
  if (i0 > GRID_W - 2) i0 = GRID_W - 2;
  if (j0 > GRID_H - 2) j0 = GRID_H - 2;
  float fx = gx - i0, fy = gy - j0;
  if (fx < 0) fx = 0; else if (fx > 1) fx = 1;
  if (fy < 0) fy = 0; else if (fy > 1) fy = 1;
  const uint8_t *r0 = g + j0 * GRID_W + i0, *r1 = r0 + GRID_W;
  float top = r0[0] + (r0[1] - r0[0]) * fx;
  float bot = r1[0] + (r1[1] - r1[0]) * fx;
  return top + (bot - top) * fy;
}

static void draw_map(void) {
  for (int my = 0; my < MASK_H; my++) {
    int sy = MAP_Y + my;
    for (int x = 0; x < MASK_W; x++) {
      /* base cartography */
      if (mask_bit(COAST_MASK, x, my)) px(x, sy, c_gray);
      else if (mask_bit(LAND_MASK, x, my) && !(x & 1) && !(sy & 1)) px(x, sy, c_dim);
      if (!s_have) continue;
      /* solid = now (with a 25%-dithered fringe at the rain edge),
       * 50% checkerboard = next 2 h */
      float vn = bilin(s_now, x, my);
      if (vn >= L_RAIN) px(x, sy, level_color(vn));
      else if (vn >= L_FRINGE) {
        if (!(x & 1) && !(sy & 1)) px(x, sy, c_rain);
      } else {
        float vf = bilin(s_fut, x, my);
        if (vf >= L_RAIN && !((x + sy) & 1)) px(x, sy, level_color(vf));
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
  /* health dot: bottom-right corner, clear of the longest rail strings */
  uint8_t dot = s_health == 0 ? c_hgrn : s_health == 1 ? c_hyel : c_hred;
  fill(191, 218, 5, 5, dot);

  graphics_release_frame_buffer(ctx, fb);
}

static void inbox(DictionaryIterator *it, void *ctx) {
  Tuple *tp;
  if ((tp = dict_find(it, MESSAGE_KEY_CELLS)) && tp->length == NCELLS) {
    memcpy(s_cells, tp->value->data, NCELLS);
    decode_cells();
  }
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
  persist_write_data(PKEY_CELLS0, s_cells, 200);
  persist_write_data(PKEY_CELLS1, s_cells + 200, 200);
  persist_write_data(PKEY_CELLS2, s_cells + 400, NCELLS - 400);
  persist_write_data(PKEY_VECS, s_vecs, sizeof s_vecs);
  Meta m = { s_temp, s_dew, s_uv, s_aqi, (uint8_t)s_health, (uint8_t)s_nvec, "" };
  strncpy(m.wind, s_wind, sizeof m.wind - 1);
  persist_write_data(PKEY_META, &m, sizeof m);
}

static void restore(void) {
  if (!persist_exists(PKEY_META)) return;
  Meta m;
  if (persist_read_data(PKEY_META, &m, sizeof m) != sizeof m) return;
  if (persist_read_data(PKEY_CELLS0, s_cells, 200) != 200) return;
  if (persist_read_data(PKEY_CELLS1, s_cells + 200, 200) != 200) return;
  if (persist_read_data(PKEY_CELLS2, s_cells + 400, NCELLS - 400) != NCELLS - 400) return;
  persist_read_data(PKEY_VECS, s_vecs, sizeof s_vecs);
  decode_cells();
  s_temp = m.temp; s_dew = m.dew; s_uv = m.uv; s_aqi = m.aqi;
  s_health = m.health;
  s_nvec = m.nvec > MAX_VECS ? MAX_VECS : m.nvec;
  memcpy(s_wind, m.wind, sizeof s_wind);
  s_wind[sizeof s_wind - 1] = 0;
  s_have = true;
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

  restore();
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
