/*
 * Zigbee ARGB LED Driver - XIAO ESP32-C6
 *
 * WS2812B strip (30 LEDs) on GPIO2, exposed as a standard ZCL
 * Color Dimmable Light endpoint. Device joins the network as a
 * router (ZCZR) and therefore also acts as a Zigbee range extender.
 *
 * Features:
 *   - On/Off, level (brightness) and color (X/Y, Hue/Sat, ColorTemp)
 *   - Smooth, non-blocking transitions (millis-based)
 *   - Last state restored from NVS on boot (instant display before join)
 *   - Factory reset via 3 s long press of BOOT button
 *
 * Build (arduino-cli, single FQBN):
 *   arduino-cli compile \
 *     -b "esp32:esp32:esp32c6:PartitionScheme=zigbee_zczr,ZigbeeMode=zczr,FlashFreq=80,FlashMode=qio,UploadSpeed=921600" \
 *     .
 *   arduino-cli upload  \
 *     -b "esp32:esp32:esp32c6:PartitionScheme=zigbee_zczr,ZigbeeMode=zczr,FlashFreq=80,FlashMode=qio,UploadSpeed=921600" \
 *     -p COM3 .
 */

#ifndef ZIGBEE_MODE_ZCZR
#error "Zigbee coordinator/router mode is not selected (set -DZIGBEE_MODE_ZCZR / ZigbeeMode=zczr)"
#endif

#include <Arduino.h>
#include <FastLED.h>
#include <Preferences.h>
#include "Zigbee.h"

// ---------- Hardware / behavior config ----------
#define LED_PIN              2
#define NUM_LEDS             97
#define LED_CHIPSET          WS2812B
#define LED_COLOR_ORDER      GRB
#define MAX_MILLIAMPS        1600          // safety cap; tune to your PSU

#define ZB_ENDPOINT          10
#define ZB_MANUFACTURER      "Eloquent Systems"
#define ZB_MODEL             "ARGB LED STRIP ESP-32-C6"
#define BUTTON_PIN           BOOT_PIN      // XIAO ESP32-C6 BOOT button = GPIO9
#define RESET_HOLD_MS        3000

#define TRANSITION_MS        400           // fade duration between target states
#define FRAME_INTERVAL_MS    20            // ~50 fps render
#define NVS_SAVE_DEBOUNCE_MS 2000
#define NVS_NAMESPACE        "argb"

// ---------- Globals ----------
CRGB                       leds[NUM_LEDS];
Preferences                prefs;
ZigbeeColorDimmableLight   zbColorLight(ZB_ENDPOINT);

struct LightState {
  bool    on    = false;
  uint8_t level = 255;
  uint8_t r     = 255;
  uint8_t g     = 255;
  uint8_t b     = 255;
};

static LightState target;                // what the network/user asked for
static uint8_t    shown_r = 0,  shown_g = 0,  shown_b = 0;   // currently displayed
static uint8_t    fade_from_r = 0, fade_from_g = 0, fade_from_b = 0;
static uint8_t    fade_to_r   = 0, fade_to_g   = 0, fade_to_b   = 0;
static uint32_t   fade_start_ms       = 0;
static bool       fading              = false;
static uint32_t   last_frame_ms       = 0;
static uint32_t   last_change_ms      = 0;
static bool       state_dirty         = false;

// Identify: drives the strip directly (overrides fade output)
static bool       identify_active     = false;

// Button (non-blocking edge tracking)
static uint32_t btn_pressed_at_ms = 0;
static bool     btn_was_down      = false;
static bool     reset_triggered   = false;

// ---------- Helpers ----------
static inline void computeFinalRgb(const LightState &s, uint8_t &fr, uint8_t &fg, uint8_t &fb) {
  if (!s.on || s.level == 0) { fr = fg = fb = 0; return; }
  fr = (uint16_t)s.r * s.level / 255;
  fg = (uint16_t)s.g * s.level / 255;
  fb = (uint16_t)s.b * s.level / 255;
}

static void startFadeToTarget() {
  uint8_t nr, ng, nb;
  computeFinalRgb(target, nr, ng, nb);
  // If we're already on our way to / sitting on the same final colour, do nothing.
  if (nr == fade_to_r && ng == fade_to_g && nb == fade_to_b &&
      nr == shown_r   && ng == shown_g   && nb == shown_b   && !fading) {
    return;
  }
  fade_from_r = shown_r; fade_from_g = shown_g; fade_from_b = shown_b;
  fade_to_r   = nr;      fade_to_g   = ng;      fade_to_b   = nb;
  fade_start_ms = millis();
  fading        = true;
  state_dirty   = true;
  last_change_ms = millis();
}

static void renderFrame(uint32_t now) {
  if (identify_active) {
    return;     // identify callback drives the strip directly while active
  }
  if (fading) {
    uint32_t elapsed = now - fade_start_ms;
    if (elapsed >= TRANSITION_MS) {
      shown_r = fade_to_r; shown_g = fade_to_g; shown_b = fade_to_b;
      fading  = false;
    } else {
      uint16_t p = (elapsed * 255UL) / TRANSITION_MS;     // 0..255
      shown_r = fade_from_r + (int16_t)(fade_to_r - fade_from_r) * (int16_t)p / 255;
      shown_g = fade_from_g + (int16_t)(fade_to_g - fade_from_g) * (int16_t)p / 255;
      shown_b = fade_from_b + (int16_t)(fade_to_b - fade_from_b) * (int16_t)p / 255;
    }
  }
  CRGB c(shown_r, shown_g, shown_b);
  for (int i = 0; i < NUM_LEDS; ++i) leds[i] = c;
  FastLED.show();
}

// ---------- NVS ----------
static void saveState() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putUChar("on",    target.on ? 1 : 0);
  prefs.putUChar("level", target.level);
  prefs.putUChar("r",     target.r);
  prefs.putUChar("g",     target.g);
  prefs.putUChar("b",     target.b);
  prefs.end();
  state_dirty = false;
}

static void loadState() {
  prefs.begin(NVS_NAMESPACE, true);
  target.on    = prefs.getUChar("on",    0) != 0;
  target.level = prefs.getUChar("level", 255);
  target.r     = prefs.getUChar("r",     255);
  target.g     = prefs.getUChar("g",     255);
  target.b     = prefs.getUChar("b",     255);
  prefs.end();
}

// ---------- Zigbee callbacks ----------
static void onRgb(bool state, uint8_t r, uint8_t g, uint8_t b, uint8_t level) {
  target.on    = state;
  target.r     = r;
  target.g     = g;
  target.b     = b;
  target.level = level;
  startFadeToTarget();
}

static void onHsv(bool state, uint8_t h, uint8_t s, uint8_t v) {
  // v already contains brightness (0-255); use level=255 so we don't double-scale.
  CHSV hsv(h, s, 255);
  CRGB rgb; hsv2rgb_rainbow(hsv, rgb);
  target.on    = state;
  target.r     = rgb.r;
  target.g     = rgb.g;
  target.b     = rgb.b;
  target.level = v;
  startFadeToTarget();
}

static void onTemp(bool state, uint8_t level, uint16_t mireds) {
  if (mireds == 0) mireds = 1;
  uint16_t kelvin = 1000000UL / mireds;
  // Approximate warm <-> cool white on RGB pixels:
  //   2000K (amber)  -> (255, 120,  30)
  //   6500K (cool)   -> (200, 220, 255)
  uint8_t r = constrain(map(kelvin, 2000, 6500, 255, 200), 0, 255);
  uint8_t g = constrain(map(kelvin, 2000, 6500, 120, 220), 0, 255);
  uint8_t b = constrain(map(kelvin, 2000, 6500,  30, 255), 0, 255);
  target.on    = state;
  target.r     = r;
  target.g     = g;
  target.b     = b;
  target.level = level;
  startFadeToTarget();
}

// Identify: library re-enters this every second while IdentifyTime counts
// down. Toggle the whole strip white/off for a 1 Hz blink. When time hits 0,
// hand control back to the renderer and fade to the user's solid state.
static void onIdentify(uint16_t time) {
  static bool blink = false;
  if (time == 0) {
    identify_active = false;
    fade_from_r = 0; fade_from_g = 0; fade_from_b = 0;
    computeFinalRgb(target, fade_to_r, fade_to_g, fade_to_b);
    fade_start_ms = millis();
    fading = true;
    return;
  }
  identify_active = true;
  blink = !blink;
  CRGB c = blink ? CRGB::White : CRGB::Black;
  for (int i = 0; i < NUM_LEDS; ++i) leds[i] = c;
  FastLED.show();
}

// ---------- Button (non-blocking) ----------
static void checkButton(uint32_t now) {
  bool down = (digitalRead(BUTTON_PIN) == LOW);
  if (down && !btn_was_down) {
    btn_pressed_at_ms = now;
    btn_was_down      = true;
    reset_triggered   = false;
  } else if (!down && btn_was_down) {
    btn_was_down = false;
  } else if (down && !reset_triggered && (now - btn_pressed_at_ms >= RESET_HOLD_MS)) {
    reset_triggered = true;
    Serial.println("Long press -> factory reset");
    // Visual cue
    for (int i = 0; i < NUM_LEDS; ++i) leds[i] = CRGB::Red;
    FastLED.show();
    prefs.begin(NVS_NAMESPACE, false);
    prefs.clear();
    prefs.end();
    delay(300);
    Zigbee.factoryReset();    // wipes Zigbee NVS and reboots
  }
}

// ---------- Arduino ----------
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\nZigbee ARGB LED Driver (XIAO ESP32-C6 / ZCZR router)");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  FastLED.addLeds<LED_CHIPSET, LED_PIN, LED_COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_MILLIAMPS);
  FastLED.clear(true);

  // Restore last state from NVS and apply immediately (no fade) so the
  // strip shows the right colour before Zigbee even comes up.
  loadState();
  computeFinalRgb(target, shown_r, shown_g, shown_b);
  fade_to_r = shown_r; fade_to_g = shown_g; fade_to_b = shown_b;
  {
    CRGB c(shown_r, shown_g, shown_b);
    for (int i = 0; i < NUM_LEDS; ++i) leds[i] = c;
    FastLED.show();
  }

  // Zigbee endpoint setup
  uint16_t caps = ZIGBEE_COLOR_CAPABILITY_HUE_SATURATION
                | ZIGBEE_COLOR_CAPABILITY_X_Y
                | ZIGBEE_COLOR_CAPABILITY_COLOR_TEMP;
  zbColorLight.setLightColorCapabilities(caps);
  // mireds = 1e6 / Kelvin; min mireds = max Kelvin and vice versa.
  zbColorLight.setLightColorTemperatureRange(1000000UL / 6500, 1000000UL / 2000);

  zbColorLight.onLightChangeRgb(onRgb);
  zbColorLight.onLightChangeHsv(onHsv);
  zbColorLight.onLightChangeTemp(onTemp);
  zbColorLight.onIdentify(onIdentify);
  zbColorLight.setManufacturerAndModel(ZB_MANUFACTURER, ZB_MODEL);

  Zigbee.addEndpoint(&zbColorLight);

  // Custom router config: bump max children (range extender role).
  esp_zb_cfg_t cfg = ZIGBEE_DEFAULT_ROUTER_CONFIG();
  cfg.nwk_cfg.zczr_cfg.max_children = 20;

  Serial.println("Starting Zigbee as ROUTER...");
  if (!Zigbee.begin(&cfg)) {
    Serial.println("Zigbee start failed - rebooting in 1s");
    delay(1000);
    ESP.restart();
  }

  Serial.print("Joining network");
  uint32_t t0 = millis();
  while (!Zigbee.connected() && millis() - t0 < 60000) {
    Serial.print('.');
    checkButton(millis());        // allow factory reset while joining
    delay(100);
  }
  Serial.println();
  Serial.println(Zigbee.connected() ? "Connected." : "Join timeout - continuing anyway.");

  // Push the restored state into the Zigbee attributes so the coordinator's
  // view matches the actual strip after a power cycle.
  zbColorLight.setLight(target.on, target.level, target.r, target.g, target.b);
}

void loop() {
  uint32_t now = millis();

  checkButton(now);

  if (now - last_frame_ms >= FRAME_INTERVAL_MS) {
    last_frame_ms = now;
    renderFrame(now);
  }

  if (state_dirty && (now - last_change_ms >= NVS_SAVE_DEBOUNCE_MS)) {
    saveState();
  }
}
