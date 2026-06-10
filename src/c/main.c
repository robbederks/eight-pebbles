#include <pebble.h>

// Eight Sleep remote for Pebble Time 2 (emery, 200x228 color).
// Up/Down nudge the current temperature level (debounced into one API call),
// Select dismisses a ringing/snoozed alarm (or refreshes when none is
// active), long Select snoozes a ringing alarm, otherwise toggles the side
// on/off. Levels are the raw Eight Sleep -100..100 scale; the UI shows the
// app's familiar -10..+10 scale (1 app unit == 10 raw units).

#define LEVEL_STEP 10
#define LEVEL_MIN (-100)
#define LEVEL_MAX 100
#define DEBOUNCE_MS 400
// Until the phone acks it's working on the command: covers BT + JS wakeup.
#define INITIAL_TIMEOUT_MS 12000
// Re-armed by the phone's 8s working heartbeats while a chain runs, so this
// only needs to cover a few missed heartbeats — not the whole chain.
#define WORKING_TIMEOUT_MS 25000
#define CLICK_REPEAT_MS 250
#define OUTBOX_RETRY_MS 300
#define STALE_AFTER_S (15 * 60)

#define ENTRANCE_MS 220
#define GAUGE_TWEEN_MS 240
// Brand splash: mark pops in, holds a beat, then flies up to dock as the
// badge while the content cascades in. Input is live the whole time.
#define SPLASH_POP_MS 200
#define SPLASH_HOLD_MS 320
#define SPLASH_FLY_MS 300
#define BADGE_FRAME GRect(5, 3, 14, 14)

enum {
  PERSIST_HAS_STATE = 1,
  PERSIST_LEVEL = 2,
  PERSIST_DEVICE_LEVEL = 3,
  PERSIST_ON = 4,
  PERSIST_PHASE = 5,
  PERSIST_DEGREES = 6,
  PERSIST_UNIT_C = 7,
  PERSIST_SIDE = 8,
  PERSIST_HAPTICS = 9,
  PERSIST_CONFIGURED = 10,
  PERSIST_UPDATED_AT = 11,
  PERSIST_SCALE_DEG = 12,
};

enum {
  CMD_REFRESH = 0,
  CMD_SET_LEVEL = 1,
  CMD_SET_POWER = 2,
  CMD_ALARM_STOP = 3,
  CMD_ALARM_SNOOZE = 4,
};

enum {
  ERR_NONE = 0,
  ERR_NO_CREDENTIALS = 1,
  ERR_AUTH_FAILED = 2,
  ERR_NETWORK = 3,
  ERR_API = 4,
};

enum {
  ALARM_NONE = 0,
  ALARM_RINGING = 1,
  ALARM_SNOOZED = 2,
};

static Window *s_window;
static StatusBarLayer *s_status_bar;
static ActionBarLayer *s_action_bar;
static TextLayer *s_side_layer;
static TextLayer *s_num_layer;
static TextLayer *s_deg_layer;
static TextLayer *s_phase_layer;
static TextLayer *s_setup_layer;
static Layer *s_gauge_layer;
static Layer *s_splash_bg;
static Layer *s_logo_layer;
static AppTimer *s_reveal_timer;

static GBitmap *s_icon_plus, *s_icon_minus, *s_icon_power, *s_icon_alarm;
static GFont s_font_big;
static bool s_icons_shown;
static bool s_select_shows_alarm;

// Canonical layer frames; animations always resolve back to these.
static GRect s_side_frame, s_num_frame, s_deg_frame, s_phase_frame,
    s_gauge_frame;
static bool s_entrance_played;

// Bed state (raw levels)
static int s_level = 0;
static int s_device_level = 0;
static bool s_on = false;
static char s_phase[16] = "";
static int s_degrees = 0;
static bool s_unit_c = true;
static char s_side[16] = "MY SIDE";
static bool s_haptics = true;
static bool s_scale_deg = false;  // adjust/display in degrees, not levels
static bool s_has_state = false;
static int s_alarm_state = ALARM_NONE;
static int s_updated_at = 0;  // epoch of last confirmed state
static bool s_stale = false;

// Session state
static bool s_configured = true;
static bool s_pending = false;      // local optimistic change not yet confirmed
static bool s_level_dirty = false;  // local level value not yet sent/confirmed
static bool s_awaiting_reply = false;
static int s_error = ERR_NONE;
static int s_last_cmd = CMD_REFRESH;
static int s_seq = 0;  // echoed back by JS to correlate replies with commands

// Outbox bookkeeping: queued commands (latest wins per domain, so a bed
// command can never displace a queued alarm dismiss or vice versa) for when
// the outbox is busy, and a snapshot of the last payload for the
// post-failure retry so an inbound status can't morph the retry into a
// different command.
static int s_queued_alarm = -1;
static int s_queued_bed = -1;
static int s_retry_cmd;
static int s_retry_level;
static bool s_retry_on;
static bool s_retried_outbox = false;
static AppTimer *s_retry_timer;

static AppTimer *s_debounce_timer;
static AppTimer *s_timeout_timer;
static AppTimer *s_sync_timer;
static AppTimer *s_boot_deadline;
static int s_sync_dots;
// Generation stamps: app_timer_reschedule can fail with the old callback
// already queued; a stale fire must be ignorable.
static int s_timeout_gen;
static int s_debounce_gen;

// Animated (displayed) gauge values, tweened toward the real state.
static int s_disp_level;
static int s_disp_device;
static Animation *s_gauge_anim;
static int s_anim_from_level, s_anim_to_level;
static int s_anim_from_dev, s_anim_to_dev;
static Animation *s_settle_anim;
static int s_banner_drawn_alarm = ALARM_NONE;

static char s_num_text[8];
static char s_deg_text[20];
static char s_phase_text[20];
static char s_glance_text[32];

static void send_cmd(int cmd);
static void update_ui(void);

// ---- Level -> degrees (anchor tables from the reference integration) ----

typedef struct {
  int16_t raw;
  int16_t deg;
} DegAnchor;

static const DegAnchor DEG_C[] = {
    {-100, 13}, {-50, 21}, {-25, 24}, {-17, 25}, {-8, 26},
    {0, 27},    {17, 30},  {50, 36},  {100, 44},
};
static const DegAnchor DEG_F[] = {
    {-100, 55}, {-72, 65}, {-49, 70}, {-40, 72}, {-26, 75}, {-18, 77},
    {0, 81},    {17, 86},  {44, 95},  {60, 100}, {92, 110}, {100, 111},
};

static int raw_to_degrees(int raw, bool celsius) {
  const DegAnchor *m = celsius ? DEG_C : DEG_F;
  const int n = celsius ? (int)ARRAY_LENGTH(DEG_C) : (int)ARRAY_LENGTH(DEG_F);
  if (raw <= m[0].raw) {
    return m[0].deg;
  }
  for (int i = 1; i < n; i++) {
    if (raw <= m[i].raw) {
      int x0 = m[i - 1].raw, y0 = m[i - 1].deg;
      int x1 = m[i].raw, y1 = m[i].deg;
      return y0 + ((raw - x0) * (y1 - y0) + (x1 - x0) / 2) / (x1 - x0);
    }
  }
  return m[n - 1].deg;
}

// Inverse lookup for degree-scale adjustment (both tables are strictly
// increasing in degrees).
static int raw_from_degrees(int deg, bool celsius) {
  const DegAnchor *m = celsius ? DEG_C : DEG_F;
  const int n = celsius ? (int)ARRAY_LENGTH(DEG_C) : (int)ARRAY_LENGTH(DEG_F);
  if (deg <= m[0].deg) {
    return m[0].raw;
  }
  for (int i = 1; i < n; i++) {
    if (deg <= m[i].deg) {
      int x0 = m[i - 1].deg, y0 = m[i - 1].raw;
      int x1 = m[i].deg, y1 = m[i].raw;
      return y0 + ((deg - x0) * (y1 - y0) + (x1 - x0) / 2) / (x1 - x0);
    }
  }
  return m[n - 1].raw;
}

static void recompute_degrees(void) {
  s_degrees = raw_to_degrees(s_level, s_unit_c);
}

// ---- Haptics: two gentle ticks for "done", one long buzz for "failed" ----

static void vibe_confirm(void) {
  static const uint32_t segments[] = {35, 70, 35};
  vibes_enqueue_custom_pattern((VibePattern){
      .durations = segments,
      .num_segments = ARRAY_LENGTH(segments),
  });
}

static void vibe_fail(void) {
  vibes_long_pulse();
}

// ---- Animations ----

static void gauge_anim_update(Animation *anim, const AnimationProgress p) {
  s_disp_level = s_anim_from_level +
                 (int)((s_anim_to_level - s_anim_from_level) * (int32_t)p /
                       ANIMATION_NORMALIZED_MAX);
  s_disp_device = s_anim_from_dev +
                  (int)((s_anim_to_dev - s_anim_from_dev) * (int32_t)p /
                        ANIMATION_NORMALIZED_MAX);
  layer_mark_dirty(s_gauge_layer);
}

static void gauge_anim_stopped(Animation *anim, bool finished, void *context) {
  if (anim == s_gauge_anim) {
    s_gauge_anim = NULL;
  }
}

static const AnimationImplementation GAUGE_ANIM_IMPL = {
    .update = gauge_anim_update,
};

static void animate_gauge(void) {
  if (s_disp_level == s_level && s_disp_device == s_device_level) {
    if (s_gauge_anim) {
      // Already displaying the target: a still-running tween would keep
      // driving toward its stale target. Stopped handler clears the pointer.
      animation_unschedule(s_gauge_anim);
    }
    layer_mark_dirty(s_gauge_layer);
    return;
  }
  if (!s_entrance_played) {
    // Window not settled yet: snap, don't tween
    s_disp_level = s_level;
    s_disp_device = s_device_level;
    layer_mark_dirty(s_gauge_layer);
    return;
  }
  if (s_gauge_anim) {
    animation_unschedule(s_gauge_anim);  // stopped handler clears the pointer
  }
  s_anim_from_level = s_disp_level;
  s_anim_to_level = s_level;
  s_anim_from_dev = s_disp_device;
  s_anim_to_dev = s_device_level;
  s_gauge_anim = animation_create();
  animation_set_implementation(s_gauge_anim, &GAUGE_ANIM_IMPL);
  animation_set_duration(s_gauge_anim, GAUGE_TWEEN_MS);
  animation_set_curve(s_gauge_anim, AnimationCurveEaseInOut);
  animation_set_handlers(
      s_gauge_anim, (AnimationHandlers){.stopped = gauge_anim_stopped}, NULL);
  animation_schedule(s_gauge_anim);
}

static void slide_to(Layer *layer, GRect from, GRect to, uint32_t delay,
                     uint32_t duration) {
  PropertyAnimation *pa = property_animation_create_layer_frame(layer, &from,
                                                                &to);
  if (!pa) {
    layer_set_frame(layer, to);
    return;
  }
  Animation *a = property_animation_get_animation(pa);
  animation_set_duration(a, duration);
  animation_set_delay(a, delay);
  animation_set_curve(a, AnimationCurveEaseOut);
  animation_schedule(a);
}

static void settle_stopped(Animation *anim, bool finished, void *context) {
  if (anim == s_settle_anim) {
    s_settle_anim = NULL;
  }
}

// A small drop-and-return of the big number: the visual twin of the
// confirmation vibe.
static void play_confirm_settle(void) {
  if (s_settle_anim || !s_entrance_played) {
    return;
  }
  Layer *nl = text_layer_get_layer(s_num_layer);
  GRect down = s_num_frame;
  down.origin.y += 4;
  PropertyAnimation *p1 =
      property_animation_create_layer_frame(nl, &s_num_frame, &down);
  PropertyAnimation *p2 =
      property_animation_create_layer_frame(nl, &down, &s_num_frame);
  if (!p1 || !p2) {
    if (p1) property_animation_destroy(p1);
    if (p2) property_animation_destroy(p2);
    layer_set_frame(nl, s_num_frame);
    return;
  }
  Animation *a1 = property_animation_get_animation(p1);
  Animation *a2 = property_animation_get_animation(p2);
  animation_set_duration(a1, 80);
  animation_set_curve(a1, AnimationCurveEaseOut);
  animation_set_duration(a2, 120);
  animation_set_curve(a2, AnimationCurveEaseOut);
  s_settle_anim = animation_sequence_create(a1, a2, NULL);
  animation_set_handlers(
      s_settle_anim, (AnimationHandlers){.stopped = settle_stopped}, NULL);
  animation_schedule(s_settle_anim);
}

static GRect offset_rect(GRect r, int dx, int dy) {
  r.origin.x += dx;
  r.origin.y += dy;
  return r;
}

// ---- Brand mark ----
// A squared-off geometric 8 whose wide bottom loop reads as a mattress.
// Drawn procedurally so it scales from splash size down to the docked
// badge; matches the store/launcher icons.

static void logo_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  const int h = b.size.h;
  const int cx = b.size.w / 2;
  const bool mini = h < 28;
  int stroke = mini ? 2 : h * 8 / 100;
  if (stroke < 3 && !mini) stroke = 3;

  const int y0 = h * 15 / 100;
  const int y1 = h * 46 / 100;
  const int y2 = h * 85 / 100;
  const int top_w = h * 40 / 100 > 5 ? h * 40 / 100 : 5;
  const int bot_w = h * 62 / 100 > 8 ? h * 62 / 100 : 8;

  graphics_context_set_stroke_width(ctx, stroke);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_round_rect(ctx, GRect(cx - top_w / 2, y0, top_w, y1 - y0),
                           mini ? 2 : h * 11 / 100);
  graphics_draw_round_rect(ctx, GRect(cx - bot_w / 2, y1, bot_w, y2 - y1),
                           mini ? 3 : h * 13 / 100);
}

static void splash_bg_update(Layer *layer, GContext *ctx) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, layer_get_bounds(layer), 0, GCornerNone);
}

// The one orchestrated moment: content settles into place, top-down,
// the instant the window appears.
static void play_entrance(void) {
  slide_to(text_layer_get_layer(s_side_layer), offset_rect(s_side_frame, 0, -24),
           s_side_frame, 0, ENTRANCE_MS);
  slide_to(text_layer_get_layer(s_num_layer), offset_rect(s_num_frame, 0, 30),
           s_num_frame, 40, ENTRANCE_MS);
  slide_to(text_layer_get_layer(s_deg_layer), offset_rect(s_deg_frame, 0, 26),
           s_deg_frame, 80, ENTRANCE_MS);
  slide_to(text_layer_get_layer(s_phase_layer),
           offset_rect(s_phase_frame, 0, 24), s_phase_frame, 110, ENTRANCE_MS);
  slide_to(s_gauge_layer, offset_rect(s_gauge_frame, 0, 28), s_gauge_frame,
           140, ENTRANCE_MS);
}

// ---- Rendering ----

static GColor accent_color(void) {
  if (!s_on) {
    return GColorDarkGray;
  }
  if (s_level > 0) {
    return PBL_IF_COLOR_ELSE(GColorOrange, GColorWhite);
  }
  if (s_level < 0) {
    return PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorWhite);
  }
  return GColorLightGray;
}

static GColor disp_fill_color(void) {
  // Fill color follows the displayed (tweening) value so the bar doesn't
  // flip hue before it crosses the center.
  if (s_disp_level > 0) {
    return PBL_IF_COLOR_ELSE(GColorOrange, GColorWhite);
  }
  return PBL_IF_COLOR_ELSE(GColorVividCerulean, GColorWhite);
}

// The command the watch is currently waiting on, or -1.
static int inflight_cmd(void) {
  if (!s_awaiting_reply) {
    return -1;
  }
  if (s_queued_alarm >= 0) {
    return s_queued_alarm;
  }
  if (s_queued_bed >= 0) {
    return s_queued_bed;
  }
  return s_last_cmd;
}

static bool syncing_now(void) {
  if (!s_configured || s_error != ERR_NONE) {
    return false;
  }
  // First-ever sync, or a deliberate refresh in flight
  return !s_has_state || inflight_cmd() == CMD_REFRESH;
}

static void sync_tick(void *context) {
  s_sync_timer = NULL;
  if (!syncing_now()) {
    return;
  }
  s_sync_dots = (s_sync_dots + 1) % 3;
  update_ui();
}

static void format_phase_text(void) {
  if (s_error == ERR_NO_CREDENTIALS) {
    strncpy(s_phase_text, "NOT LOGGED IN", sizeof(s_phase_text));
  } else if (s_error == ERR_AUTH_FAILED) {
    strncpy(s_phase_text, "CHECK LOGIN", sizeof(s_phase_text));
  } else if (s_error == ERR_NETWORK) {
    strncpy(s_phase_text, "NO CONNECTION", sizeof(s_phase_text));
  } else if (s_error == ERR_API) {
    strncpy(s_phase_text, "BED ERROR", sizeof(s_phase_text));
  } else if (inflight_cmd() == CMD_ALARM_STOP) {
    strncpy(s_phase_text, "DISMISSING", sizeof(s_phase_text));
  } else if (inflight_cmd() == CMD_ALARM_SNOOZE) {
    strncpy(s_phase_text, "SNOOZING", sizeof(s_phase_text));
  } else if (s_alarm_state == ALARM_RINGING) {
    strncpy(s_phase_text, "ALARM", sizeof(s_phase_text));
  } else if (s_alarm_state == ALARM_SNOOZED) {
    strncpy(s_phase_text, "SNOOZED", sizeof(s_phase_text));
  } else if (syncing_now()) {
    snprintf(s_phase_text, sizeof(s_phase_text), "SYNCING%.*s", s_sync_dots,
             "..");
  } else if (!s_on) {
    strncpy(s_phase_text, "OFF", sizeof(s_phase_text));
  } else if (s_phase[0]) {
    // "smart:bedtime" arrives as just the phase part, e.g. "bedtime"
    snprintf(s_phase_text, sizeof(s_phase_text), "%s", s_phase);
    for (char *p = s_phase_text; *p; p++) {
      if (*p >= 'a' && *p <= 'z') {
        *p -= 32;
      }
    }
  } else {
    strncpy(s_phase_text, "SMART", sizeof(s_phase_text));
  }
  s_phase_text[sizeof(s_phase_text) - 1] = '\0';
}

static void format_age_text(char *buf, size_t len) {
  int age = (int)time(NULL) - s_updated_at;
  if (age < 0) {
    age = 0;
  }
  if (age < 3600) {
    snprintf(buf, len, "as of %dm ago", age / 60);
  } else if (age < 86400) {
    snprintf(buf, len, "as of %dh ago", age / 3600);
  } else {
    snprintf(buf, len, "as of %dd ago", age / 86400);
  }
}

static void set_action_bar_icons(bool shown) {
  bool want_alarm = s_alarm_state != ALARM_NONE;
  if (shown && s_icons_shown && s_select_shows_alarm == want_alarm) {
    return;
  }
  if (!shown && !s_icons_shown) {
    return;
  }
  s_icons_shown = shown;
  if (shown) {
    action_bar_layer_set_icon(s_action_bar, BUTTON_ID_UP, s_icon_plus);
    action_bar_layer_set_icon(s_action_bar, BUTTON_ID_SELECT,
                              want_alarm ? s_icon_alarm : s_icon_power);
    action_bar_layer_set_icon(s_action_bar, BUTTON_ID_DOWN, s_icon_minus);
    s_select_shows_alarm = want_alarm;
  } else {
    action_bar_layer_clear_icon(s_action_bar, BUTTON_ID_UP);
    action_bar_layer_clear_icon(s_action_bar, BUTTON_ID_SELECT);
    action_bar_layer_clear_icon(s_action_bar, BUTTON_ID_DOWN);
  }
}

static void update_ui(void) {
  bool setup = !s_configured;
  layer_set_hidden(text_layer_get_layer(s_setup_layer), !setup);
  layer_set_hidden(text_layer_get_layer(s_side_layer), setup);
  layer_set_hidden(text_layer_get_layer(s_num_layer), setup);
  layer_set_hidden(text_layer_get_layer(s_deg_layer), setup);
  layer_set_hidden(text_layer_get_layer(s_phase_layer), setup);
  layer_set_hidden(s_gauge_layer, setup);
  set_action_bar_icons(!setup);
  if (setup) {
    action_bar_layer_set_background_color(s_action_bar, GColorDarkGray);
    return;
  }

  int app_units = s_level / LEVEL_STEP;
  if (!s_has_state && !s_pending) {
    strncpy(s_num_text, "--", sizeof(s_num_text));
    s_num_text[sizeof(s_num_text) - 1] = '\0';
  } else if (s_scale_deg) {
    snprintf(s_num_text, sizeof(s_num_text), "%d°", s_degrees);
  } else if (app_units > 0) {
    snprintf(s_num_text, sizeof(s_num_text), "+%d", app_units);
  } else {
    snprintf(s_num_text, sizeof(s_num_text), "%d", app_units);
  }
  text_layer_set_text(s_num_layer, s_num_text);

  GColor num_color;
  if (!s_on) {
    num_color = GColorDarkGray;  // off: ghosted but legible at 64px
  } else if (s_pending || s_stale) {
    num_color = GColorLightGray;  // in flight, or old enough to doubt
  } else {
    num_color = GColorWhite;
  }
  text_layer_set_text_color(s_num_layer, num_color);

  if (s_stale && !s_pending && s_has_state) {
    format_age_text(s_deg_text, sizeof(s_deg_text));
  } else if (s_has_state || s_pending) {
    if (s_scale_deg) {
      // Big number carries the degrees; the small line shows unit + level
      snprintf(s_deg_text, sizeof(s_deg_text), "°%c · %s%d",
               s_unit_c ? 'C' : 'F', app_units > 0 ? "+" : "", app_units);
    } else {
      snprintf(s_deg_text, sizeof(s_deg_text), "%d°%c", s_degrees,
               s_unit_c ? 'C' : 'F');
    }
  } else {
    s_deg_text[0] = '\0';
  }
  text_layer_set_text(s_deg_layer, s_deg_text);

  format_phase_text();
  text_layer_set_text(s_phase_layer, s_phase_text);
  if (s_error != ERR_NONE) {
    // Banner: error must not look like the warm accent color
    text_layer_set_background_color(
        s_phase_layer, PBL_IF_COLOR_ELSE(GColorSunsetOrange, GColorWhite));
    text_layer_set_text_color(s_phase_layer, GColorBlack);
  } else if (s_alarm_state == ALARM_RINGING) {
    text_layer_set_background_color(s_phase_layer,
                                    PBL_IF_COLOR_ELSE(GColorRed, GColorWhite));
    text_layer_set_text_color(s_phase_layer,
                              PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
  } else if (s_alarm_state == ALARM_SNOOZED) {
    text_layer_set_background_color(
        s_phase_layer, PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorWhite));
    text_layer_set_text_color(s_phase_layer, GColorBlack);
  } else {
    text_layer_set_background_color(s_phase_layer, GColorClear);
    text_layer_set_text_color(s_phase_layer,
                              s_on ? accent_color() : GColorLightGray);
  }

  // Slide the banner in from the action bar side when an alarm starts
  if (s_alarm_state != ALARM_NONE && s_banner_drawn_alarm == ALARM_NONE &&
      s_entrance_played && s_error == ERR_NONE) {
    slide_to(text_layer_get_layer(s_phase_layer),
             offset_rect(s_phase_frame, s_phase_frame.size.w, 0),
             s_phase_frame, 0, GAUGE_TWEEN_MS);
  }
  s_banner_drawn_alarm = s_alarm_state;

  if (syncing_now() && !s_sync_timer) {
    s_sync_timer = app_timer_register(450, sync_tick, NULL);
  }

  text_layer_set_text(s_side_layer, s_side);
  action_bar_layer_set_background_color(s_action_bar, accent_color());
  animate_gauge();
}

static void gauge_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  const int w = b.size.w;
  const int cy = b.size.h / 2;
  const int track_h = 6;
  const int half = w / 2;

  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(0, cy - track_h / 2, w, track_h), 2,
                     GCornersAll);

  if (s_on && s_disp_level != 0) {
    int fill = s_disp_level * half / LEVEL_MAX;
    int fw = fill > 0 ? fill : -fill;
    graphics_context_set_fill_color(ctx, disp_fill_color());
    GRect fr = fill > 0 ? GRect(half, cy - track_h / 2, fw, track_h)
                        : GRect(half - fw, cy - track_h / 2, fw, track_h);
    graphics_fill_rect(ctx, fr, fw >= 5 ? 2 : 0,
                       fill > 0 ? GCornersRight : GCornersLeft);
  }

  // Center notch
  graphics_context_set_fill_color(ctx, GColorLightGray);
  graphics_fill_rect(ctx, GRect(half - 1, cy - 7, 2, 14), 0, GCornerNone);

  // Where the bed actually is right now, ringed for separation from the fill
  if (s_has_state && s_on) {
    int dev_x = half + s_disp_device * half / LEVEL_MAX;
    if (dev_x < 5) dev_x = 5;
    if (dev_x > w - 5) dev_x = w - 5;
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, GPoint(dev_x, cy), 5);
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, GPoint(dev_x, cy), 4);
  }
}

static void persist_state(void) {
  persist_write_bool(PERSIST_HAS_STATE, s_has_state);
  persist_write_int(PERSIST_LEVEL, s_level);
  persist_write_int(PERSIST_DEVICE_LEVEL, s_device_level);
  persist_write_bool(PERSIST_ON, s_on);
  persist_write_string(PERSIST_PHASE, s_phase);
  persist_write_int(PERSIST_DEGREES, s_degrees);
  persist_write_bool(PERSIST_UNIT_C, s_unit_c);
  persist_write_string(PERSIST_SIDE, s_side);
  persist_write_bool(PERSIST_HAPTICS, s_haptics);
  persist_write_bool(PERSIST_SCALE_DEG, s_scale_deg);
  s_updated_at = (int)time(NULL);
  s_stale = false;
  persist_write_int(PERSIST_UPDATED_AT, s_updated_at);
}

static void restore_state(void) {
  if (persist_exists(PERSIST_CONFIGURED)) {
    s_configured = persist_read_bool(PERSIST_CONFIGURED);
  }
  if (!persist_exists(PERSIST_HAS_STATE)) {
    return;
  }
  s_has_state = persist_read_bool(PERSIST_HAS_STATE);
  s_level = persist_read_int(PERSIST_LEVEL);
  s_device_level = persist_read_int(PERSIST_DEVICE_LEVEL);
  s_on = persist_read_bool(PERSIST_ON);
  persist_read_string(PERSIST_PHASE, s_phase, sizeof(s_phase));
  s_unit_c = persist_read_bool(PERSIST_UNIT_C);
  persist_read_string(PERSIST_SIDE, s_side, sizeof(s_side));
  s_haptics = persist_read_bool(PERSIST_HAPTICS);
  s_scale_deg = persist_read_bool(PERSIST_SCALE_DEG);
  s_updated_at = persist_read_int(PERSIST_UPDATED_AT);
  s_stale = s_has_state && s_updated_at > 0 &&
            ((int)time(NULL) - s_updated_at) > STALE_AFTER_S;
  recompute_degrees();
}

// ---- AppMessage ----

static void fail_network(void) {
  s_error = ERR_NETWORK;
  if (s_haptics) {
    vibe_fail();
  }
}

static void timeout_fired(void *context) {
  if ((int)(intptr_t)context != s_timeout_gen) {
    return;  // stale fire from a superseded arm
  }
  s_timeout_timer = NULL;
  s_awaiting_reply = false;
  s_pending = false;
  s_level_dirty = false;
  s_queued_alarm = -1;
  s_queued_bed = -1;
  fail_network();
  update_ui();
}

static void cancel_timeout(void) {
  s_timeout_gen++;  // invalidate any already-queued fire
  if (s_timeout_timer) {
    app_timer_cancel(s_timeout_timer);
    s_timeout_timer = NULL;
  }
}

static void arm_timeout(uint32_t ms) {
  cancel_timeout();
  s_timeout_timer =
      app_timer_register(ms, timeout_fired, (void *)(intptr_t)s_timeout_gen);
}

static bool is_alarm_cmd(int cmd) {
  return cmd == CMD_ALARM_STOP || cmd == CMD_ALARM_SNOOZE;
}

static void queue_cmd(int cmd) {
  if (is_alarm_cmd(cmd)) {
    s_queued_alarm = cmd;
  } else {
    s_queued_bed = cmd;
  }
}

// Sends a fixed payload. send_cmd() snapshots the live state; the retry path
// resends the same snapshot so a late inbound status can't change what is
// retried.
static void send_payload(int cmd, int level, bool on) {
  if (s_retry_timer) {
    if (is_alarm_cmd(cmd) == is_alarm_cmd(s_retry_cmd)) {
      app_timer_cancel(s_retry_timer);  // same domain: latest wins
      s_retry_timer = NULL;
    } else {
      // Cross-domain: let the pending retry go first and park this command;
      // it flushes from the retry's outbox_sent. Costs at most one
      // OUTBOX_RETRY_MS — never silently drops either command.
      queue_cmd(cmd);
      s_awaiting_reply = true;
      arm_timeout(INITIAL_TIMEOUT_MS);
      return;
    }
  }
  DictionaryIterator *iter;
  AppMessageResult res = app_message_outbox_begin(&iter);
  if (res == APP_MSG_BUSY) {
    // Previous message still in flight: remember the command and resend from
    // outbox_sent/outbox_failed. The deferred send re-reads the live state,
    // so it carries the latest user intent (latest wins per domain).
    queue_cmd(cmd);
    s_awaiting_reply = true;
    arm_timeout(INITIAL_TIMEOUT_MS);
    return;
  }
  if (res != APP_MSG_OK) {
    s_queued_alarm = -1;
    s_queued_bed = -1;
    s_awaiting_reply = false;
    s_pending = false;
    s_level_dirty = false;
    cancel_timeout();
    fail_network();
    update_ui();
    return;
  }
  s_seq++;
  dict_write_int32(iter, MESSAGE_KEY_Cmd, cmd);
  dict_write_int32(iter, MESSAGE_KEY_Seq, s_seq);
  if (cmd == CMD_SET_LEVEL) {
    dict_write_int32(iter, MESSAGE_KEY_Level, level);
  } else if (cmd == CMD_SET_POWER) {
    dict_write_int32(iter, MESSAGE_KEY_State, on ? 1 : 0);
  }
  app_message_outbox_send();
  s_retry_cmd = cmd;
  s_retry_level = level;
  s_retry_on = on;
  s_last_cmd = cmd;
  s_awaiting_reply = true;
  arm_timeout(INITIAL_TIMEOUT_MS);
}

static void send_cmd(int cmd) {
  s_retried_outbox = false;  // a fresh command gets its own single retry
  send_payload(cmd, s_level, s_on);
}

// Alarm slot first: stopping a ringing bed wins over a temperature tweak.
// The bed slot goes out on the alarm command's outbox_sent.
static void flush_queued_cmd(void) {
  if (s_queued_alarm >= 0) {
    int cmd = s_queued_alarm;
    s_queued_alarm = -1;
    send_cmd(cmd);
  } else if (s_queued_bed >= 0) {
    int cmd = s_queued_bed;
    s_queued_bed = -1;
    send_cmd(cmd);
  }
}

static void outbox_retry(void *context) {
  s_retry_timer = NULL;
  send_payload(s_retry_cmd, s_retry_level, s_retry_on);
}

static void outbox_failed(DictionaryIterator *iter, AppMessageResult reason,
                          void *context) {
  if (!s_retried_outbox) {
    s_retried_outbox = true;
    s_retry_timer = app_timer_register(OUTBOX_RETRY_MS, outbox_retry, NULL);
    return;
  }
  s_retried_outbox = false;
  if (s_queued_alarm >= 0 || s_queued_bed >= 0) {
    // A newer command supersedes the failed one
    flush_queued_cmd();
    return;
  }
  s_awaiting_reply = false;
  s_pending = false;
  s_level_dirty = false;
  cancel_timeout();
  fail_network();
  update_ui();
}

static void outbox_sent(DictionaryIterator *iter, void *context) {
  s_retried_outbox = false;
  flush_queued_cmd();
}

static void inbox_received(DictionaryIterator *iter, void *context) {
  Tuple *t;
  bool is_status = false;
  int error = ERR_NONE;

  if ((t = dict_find(iter, MESSAGE_KEY_Configured))) {
    bool cfg = t->value->int32 != 0;
    if (cfg != s_configured || !persist_exists(PERSIST_CONFIGURED)) {
      persist_write_bool(PERSIST_CONFIGURED, cfg);
    }
    s_configured = cfg;
  }
  if ((t = dict_find(iter, MESSAGE_KEY_Error))) {
    error = t->value->int32;
    is_status = true;
  }

  // A status echoing the latest Seq is the reply to our in-flight command;
  // anything else (the ready push, stale replies) is a passive update.
  bool seq_match = false;
  if ((t = dict_find(iter, MESSAGE_KEY_Seq))) {
    seq_match = s_awaiting_reply && t->value->int32 == s_seq;
  }
  // A queued command is newer user intent with no seq assigned yet; until
  // it is flushed, the in-flight reply must not confirm or clobber anything.
  bool correlated = seq_match && s_queued_alarm < 0 && s_queued_bed < 0;

  // Lightweight progress ack: the phone has the command and is talking to
  // the API. Re-arm the timeout with the full network budget.
  if ((t = dict_find(iter, MESSAGE_KEY_Working))) {
    if (seq_match) {
      arm_timeout(WORKING_TIMEOUT_MS);
    }
    return;
  }

  if (is_status && s_boot_deadline) {
    // Any status settles the launch; the boot deadline is no longer needed
    app_timer_cancel(s_boot_deadline);
    s_boot_deadline = NULL;
  }

  if (is_status && error == ERR_NONE) {
    // Don't clobber a level the user is still adjusting
    if (!s_debounce_timer && (correlated || !s_level_dirty)) {
      if ((t = dict_find(iter, MESSAGE_KEY_Level))) {
        s_level = t->value->int32;
      }
    }
    // Don't let a stale status visually revert an optimistic power toggle
    if (correlated || !s_pending) {
      if ((t = dict_find(iter, MESSAGE_KEY_State))) {
        s_on = t->value->int32 != 0;
        s_has_state = true;
      }
      if ((t = dict_find(iter, MESSAGE_KEY_Phase))) {
        strncpy(s_phase, t->value->cstring, sizeof(s_phase));
        s_phase[sizeof(s_phase) - 1] = '\0';
      }
    }
    if ((t = dict_find(iter, MESSAGE_KEY_DeviceLevel))) {
      s_device_level = t->value->int32;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_Unit))) {
      s_unit_c = t->value->int32 != 0;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_SideName))) {
      strncpy(s_side, t->value->cstring, sizeof(s_side));
      s_side[sizeof(s_side) - 1] = '\0';
    }
    if ((t = dict_find(iter, MESSAGE_KEY_Haptics))) {
      s_haptics = t->value->int32 != 0;
    }
    if ((t = dict_find(iter, MESSAGE_KEY_Scale))) {
      s_scale_deg = t->value->int32 != 0;
    }
    // Omitted when the phone couldn't check alarms; keep the last value then
    if ((t = dict_find(iter, MESSAGE_KEY_Alarm))) {
      s_alarm_state = t->value->int32;
    }
    recompute_degrees();
  }

  if (is_status && correlated) {
    cancel_timeout();
    s_awaiting_reply = false;
    if (error == ERR_NONE) {
      s_error = ERR_NONE;
      if (!s_debounce_timer) {
        s_pending = false;
        s_level_dirty = false;
        // Persist only fully-confirmed state: with a debounce still armed,
        // s_level holds a not-yet-sent value that must not reach the glance.
        persist_state();
      }
      if (s_haptics && s_last_cmd != CMD_REFRESH) {
        vibe_confirm();
      }
      if (s_last_cmd != CMD_REFRESH) {
        play_confirm_settle();
      }
    } else {
      s_error = error;
      s_pending = false;
      s_level_dirty = false;
      if (s_haptics) {
        vibe_fail();
      }
    }
    s_last_cmd = CMD_REFRESH;
  } else if (is_status && error == ERR_NONE && !s_pending &&
             !s_debounce_timer && s_has_state) {
    // Passive update with no local dirt: fresh server truth, worth keeping
    s_error = ERR_NONE;
    persist_state();
  } else if (is_status && error != ERR_NONE && !s_awaiting_reply) {
    // Unsolicited failure (e.g. the launch refresh): show it
    s_error = error;
  }

  light_enable_interaction();
  update_ui();
}

// ---- Clicks ----

static void cancel_debounce(void) {
  s_debounce_gen++;  // invalidate any already-queued fire
  if (s_debounce_timer) {
    app_timer_cancel(s_debounce_timer);
    s_debounce_timer = NULL;
  }
}

static void debounce_fired(void *context) {
  if ((int)(intptr_t)context != s_debounce_gen) {
    return;  // stale fire from a superseded arm
  }
  s_debounce_timer = NULL;
  send_cmd(CMD_SET_LEVEL);
}

static void nudge(int dir) {
  if (!s_configured) {
    return;
  }
  if (s_scale_deg) {
    // Adjust by one displayed degree; the API still takes raw levels
    const DegAnchor *m = s_unit_c ? DEG_C : DEG_F;
    const int n =
        s_unit_c ? (int)ARRAY_LENGTH(DEG_C) : (int)ARRAY_LENGTH(DEG_F);
    int next_deg = s_degrees + dir;
    if (next_deg > m[n - 1].deg) next_deg = m[n - 1].deg;
    if (next_deg < m[0].deg) next_deg = m[0].deg;
    s_level = raw_from_degrees(next_deg, s_unit_c);
    s_degrees = next_deg;
  } else {
    int next = s_level + dir * LEVEL_STEP;
    if (next > LEVEL_MAX) next = LEVEL_MAX;
    if (next < LEVEL_MIN) next = LEVEL_MIN;
    s_level = next;
    recompute_degrees();
  }
  s_on = true;  // nudging while off turns the side on (JS sends smart first)
  s_error = ERR_NONE;
  s_pending = true;
  s_level_dirty = true;
  cancel_debounce();
  s_debounce_timer = app_timer_register(DEBOUNCE_MS, debounce_fired,
                                        (void *)(intptr_t)s_debounce_gen);
  update_ui();
}

static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  nudge(+1);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  nudge(-1);
}

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (!s_configured) {
    return;
  }
  s_error = ERR_NONE;
  // Dismiss an active alarm; with none, a refresh is the harmless default
  // (and the recovery gesture for stale state).
  send_cmd(s_alarm_state != ALARM_NONE ? CMD_ALARM_STOP : CMD_REFRESH);
  update_ui();
}

static void select_long_click_handler(ClickRecognizerRef recognizer,
                                      void *context) {
  if (!s_configured) {
    return;
  }
  if (s_alarm_state == ALARM_RINGING) {
    s_error = ERR_NONE;
    send_cmd(CMD_ALARM_SNOOZE);
    update_ui();
    return;
  }
  if (s_debounce_timer) {
    cancel_debounce();
    s_level_dirty = false;  // abandoned nudge; let the reply's Level resync
  }
  s_on = !s_on;
  s_error = ERR_NONE;
  s_pending = true;
  send_cmd(CMD_SET_POWER);
  update_ui();
}

static void click_config_provider(void *context) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, CLICK_REPEAT_MS,
                                          up_click_handler);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, CLICK_REPEAT_MS,
                                          down_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, select_long_click_handler,
                              NULL);
}

// ---- Window ----

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  const int content_w = bounds.size.w - ACTION_BAR_WIDTH;

  window_set_background_color(window, GColorBlack);

  s_action_bar = action_bar_layer_create();
  s_icon_plus = gbitmap_create_with_resource(RESOURCE_ID_ICON_PLUS);
  s_icon_minus = gbitmap_create_with_resource(RESOURCE_ID_ICON_MINUS);
  s_icon_power = gbitmap_create_with_resource(RESOURCE_ID_ICON_POWER);
  s_icon_alarm = gbitmap_create_with_resource(RESOURCE_ID_ICON_ALARM);
  action_bar_layer_set_click_config_provider(s_action_bar,
                                             click_config_provider);
  action_bar_layer_add_to_window(s_action_bar, window);

  const int top = STATUS_BAR_LAYER_HEIGHT;

  s_side_frame = GRect(0, top + 4, content_w, 22);
  s_side_layer = text_layer_create(s_side_frame);
  text_layer_set_font(s_side_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_color(s_side_layer, GColorLightGray);
  text_layer_set_background_color(s_side_layer, GColorClear);
  text_layer_set_text_alignment(s_side_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_side_layer));

  s_font_big = fonts_load_custom_font(
      resource_get_handle(RESOURCE_ID_FONT_BIG_64));
  s_num_frame = GRect(0, top + 28, content_w, 78);
  s_num_layer = text_layer_create(s_num_frame);
  text_layer_set_font(s_num_layer, s_font_big);
  text_layer_set_text_color(s_num_layer, GColorWhite);
  text_layer_set_background_color(s_num_layer, GColorClear);
  text_layer_set_text_alignment(s_num_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_num_layer));

  s_deg_frame = GRect(0, top + 112, content_w, 24);
  s_deg_layer = text_layer_create(s_deg_frame);
  text_layer_set_font(s_deg_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_text_color(s_deg_layer, GColorLightGray);
  text_layer_set_background_color(s_deg_layer, GColorClear);
  text_layer_set_text_alignment(s_deg_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_deg_layer));

  s_phase_frame = GRect(0, top + 140, content_w, 26);
  s_phase_layer = text_layer_create(s_phase_frame);
  text_layer_set_font(s_phase_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_background_color(s_phase_layer, GColorClear);
  text_layer_set_text_alignment(s_phase_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_phase_layer));

  s_gauge_frame = GRect(12, top + 174, content_w - 24, 18);
  s_gauge_layer = layer_create(s_gauge_frame);
  layer_set_update_proc(s_gauge_layer, gauge_update_proc);
  layer_add_child(root, s_gauge_layer);

  s_setup_layer = text_layer_create(GRect(8, top + 40, content_w - 16, 140));
  text_layer_set_font(s_setup_layer,
                      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_text_color(s_setup_layer, GColorWhite);
  text_layer_set_background_color(s_setup_layer, GColorClear);
  text_layer_set_text_alignment(s_setup_layer, GTextAlignmentCenter);
  text_layer_set_text(s_setup_layer,
                      "Log in to Eight Sleep in the app settings on your phone");
  layer_add_child(root, text_layer_get_layer(s_setup_layer));

  // The status bar goes ABOVE the content: its opaque background masks the
  // side label during the entrance slide, so it can never overlap the time.
  s_status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_status_bar, GColorBlack, GColorLightGray);
  status_bar_layer_set_separator_mode(s_status_bar,
                                      StatusBarLayerSeparatorModeNone);
  layer_set_frame(status_bar_layer_get_layer(s_status_bar),
                  GRect(0, 0, content_w, STATUS_BAR_LAYER_HEIGHT));
  layer_add_child(root, status_bar_layer_get_layer(s_status_bar));

  // Gauge starts at the restored values; the entrance slides layers in
  s_disp_level = s_level;
  s_disp_device = s_device_level;
  s_banner_drawn_alarm = s_alarm_state;

  // Park the content at its entrance offsets so the appear animation slides
  // everything into place instead of jumping away first.
  if (!s_entrance_played && s_configured) {
    layer_set_frame(text_layer_get_layer(s_side_layer),
                    offset_rect(s_side_frame, 0, -24));
    layer_set_frame(text_layer_get_layer(s_num_layer),
                    offset_rect(s_num_frame, 0, 30));
    layer_set_frame(text_layer_get_layer(s_deg_layer),
                    offset_rect(s_deg_frame, 0, 26));
    layer_set_frame(text_layer_get_layer(s_phase_layer),
                    offset_rect(s_phase_frame, 0, 24));
    layer_set_frame(s_gauge_layer, offset_rect(s_gauge_frame, 0, 28));
  }

  // Splash on top of everything (including the action bar): black backdrop
  // plus the mark. Purely visual — clicks land underneath the whole time.
  s_splash_bg = layer_create(bounds);
  layer_set_update_proc(s_splash_bg, splash_bg_update);
  layer_add_child(root, s_splash_bg);
  s_logo_layer = layer_create(GRect(0, 0, bounds.size.w, bounds.size.h));
  layer_set_update_proc(s_logo_layer, logo_update_proc);
  layer_add_child(root, s_logo_layer);
  if (s_entrance_played) {
    // Re-entry (config page return): no splash, badge straight home
    layer_set_hidden(s_splash_bg, true);
    layer_set_frame(s_logo_layer, BADGE_FRAME);
  }

  update_ui();
}

static void reveal_fired(void *context) {
  s_reveal_timer = NULL;
  layer_set_hidden(s_splash_bg, true);
  // The mark flies up and docks as the badge while the content cascades in
  slide_to(s_logo_layer, layer_get_frame(s_logo_layer), BADGE_FRAME, 0,
           SPLASH_FLY_MS);
  if (s_configured) {
    play_entrance();
  }
}

static void window_appear(Window *window) {
  if (s_entrance_played) {
    return;
  }
  s_entrance_played = true;

  GRect bounds = layer_get_bounds(window_get_root_layer(window));
  const GRect logo_big =
      GRect((bounds.size.w - 88) / 2, (bounds.size.h - 100) / 2 - 4, 88, 100);
  const GRect logo_small = GRect(logo_big.origin.x + 12, logo_big.origin.y + 14,
                                 logo_big.size.w - 24, logo_big.size.h - 28);
  layer_set_frame(s_logo_layer, logo_small);
  slide_to(s_logo_layer, logo_small, logo_big, 0, SPLASH_POP_MS);
  s_reveal_timer = app_timer_register(SPLASH_POP_MS + SPLASH_HOLD_MS,
                                      reveal_fired, NULL);
}

static void window_unload(Window *window) {
  animation_unschedule_all();
  if (s_sync_timer) {
    app_timer_cancel(s_sync_timer);
    s_sync_timer = NULL;
  }
  if (s_reveal_timer) {
    app_timer_cancel(s_reveal_timer);
    s_reveal_timer = NULL;
  }
  layer_destroy(s_splash_bg);
  layer_destroy(s_logo_layer);
  status_bar_layer_destroy(s_status_bar);
  action_bar_layer_destroy(s_action_bar);
  gbitmap_destroy(s_icon_plus);
  gbitmap_destroy(s_icon_minus);
  gbitmap_destroy(s_icon_power);
  gbitmap_destroy(s_icon_alarm);
  text_layer_destroy(s_side_layer);
  text_layer_destroy(s_num_layer);
  text_layer_destroy(s_deg_layer);
  text_layer_destroy(s_phase_layer);
  text_layer_destroy(s_setup_layer);
  layer_destroy(s_gauge_layer);
  fonts_unload_custom_font(s_font_big);
}

// ---- App glance ----

// Built from persisted (reply-confirmed) state, not live globals: a nudge
// abandoned mid-debounce or one that errored must not reach the launcher.
static void update_app_glance(AppGlanceReloadSession *session, size_t limit,
                              void *context) {
  if (limit < 1 || !s_configured || !persist_read_bool(PERSIST_HAS_STATE)) {
    return;
  }
  if (persist_read_bool(PERSIST_ON)) {
    int app_units = (int)persist_read_int(PERSIST_LEVEL) / LEVEL_STEP;
    int degrees = (int)persist_read_int(PERSIST_DEGREES);
    char unit = persist_read_bool(PERSIST_UNIT_C) ? 'C' : 'F';
    if (persist_read_bool(PERSIST_SCALE_DEG)) {
      snprintf(s_glance_text, sizeof(s_glance_text), "%d°%c · %s%d", degrees,
               unit, app_units > 0 ? "+" : "", app_units);
    } else {
      snprintf(s_glance_text, sizeof(s_glance_text), "%s%d · %d°%c",
               app_units > 0 ? "+" : "", app_units, degrees, unit);
    }
  } else {
    snprintf(s_glance_text, sizeof(s_glance_text), "Off");
  }
  const AppGlanceSlice slice = {
      .layout = {.icon = APP_GLANCE_SLICE_DEFAULT_ICON,
                 .subtitle_template_string = s_glance_text},
      .expiration_time = APP_GLANCE_SLICE_NO_EXPIRATION,
  };
  app_glance_add_slice(session, slice);
}

// ---- Init ----

// Without persisted state, no command is in flight at launch (the phone
// pushes the first status unprompted), so nothing would ever bound the
// SYNCING screen if the phone never answers.
static void boot_deadline_fired(void *context) {
  s_boot_deadline = NULL;
  if (s_configured && !s_has_state && !s_awaiting_reply && !s_pending &&
      s_error == ERR_NONE) {
    fail_network();
    update_ui();
  }
}

// Keep the staleness verdict current while the app sits open; persist_state
// resets it whenever fresh data arrives.
static void minute_tick(struct tm *tick_time, TimeUnits units_changed) {
  bool stale = s_has_state && s_updated_at > 0 &&
               ((int)time(NULL) - s_updated_at) > STALE_AFTER_S;
  if (stale != s_stale || stale) {
    s_stale = stale;
    update_ui();
  }
}

static void init(void) {
  restore_state();

  s_window = window_create();
  window_set_window_handlers(s_window, (WindowHandlers){
                                           .load = window_load,
                                           .appear = window_appear,
                                           .unload = window_unload,
                                       });

  app_message_register_inbox_received(inbox_received);
  app_message_register_outbox_failed(outbox_failed);
  app_message_register_outbox_sent(outbox_sent);
  app_message_open(512, 128);

  window_stack_push(s_window, true);
  tick_timer_service_subscribe(MINUTE_UNIT, minute_tick);
  if (s_configured && !s_has_state) {
    s_boot_deadline =
        app_timer_register(INITIAL_TIMEOUT_MS, boot_deadline_fired, NULL);
  }
}

static void deinit(void) {
  tick_timer_service_unsubscribe();
  app_glance_reload(update_app_glance, NULL);
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
