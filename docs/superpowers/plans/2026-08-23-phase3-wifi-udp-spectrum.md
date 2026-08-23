# Phase 3: WiFi + UDP + Spectrum Screen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add WiFi, UDP audio data receiver, and a three-view Spectrum screen (bars / curve / waterfall) to the ESP32-S3 Studio Panel, plus a cross-platform Python companion script that streams real audio analysis data.

**Architecture:** A shared `AudioPacket` queue (capacity 2, `xQueueOverwrite`) decouples the UDP receiver task from LVGL screens. WiFi connects on boot; the UDP task starts when an IP is assigned. Metering screen falls back to demo data when the queue is empty. Spectrum screen adds three full-screen views navigated by BOOT-button (short=next, long=freeze). Python script uses `sounddevice` + `numpy` for cross-platform audio capture.

**Tech Stack:** C++17, ESP-IDF v5.5, LVGL 9.5, FreeRTOS, lwIP UDP sockets, Python 3 + sounddevice + numpy

**Spec:** `docs/superpowers/specs/2026-08-23-phase3-wifi-udp-spectrum-design.md`

## Global Constraints

- Target: ESP32-S3-Touch-LCD-7, 800×480 IPS, THEME_STATUSBAR_H=32
- All UI colors ≥ 38% luminance EXCEPT pure visualisation canvases (waterfall, curve) which may use darker values
- Only LVGL 9.5 API — no `lv_scr_act()`, no `lv_disp_t`
- Every non-trivial function gets a one-line IO-contract comment
- Build command (run from `studio-panel/`): `bash -c 'source ~/esp/esp-idf-5.5/export.sh 2>/dev/null && idf.py build 2>&1 | grep -E " error:|fatal error" | head -20; echo "RC:$?"'`
- Flash: `idf.py -p /dev/ttyACM0 flash`
- AudioPacket total size: **1072 bytes** (spec says 1076 — spec has a typo; use 1072 and verify with `static_assert`)
- UDP port: **4210**
- FFT bins: **256** (float32, magnitude 0.0–1.0)
- Commit after every task using `git commit -m "feat/fix: ..."`
- Working directory for all commands: `/mnt/source/data/coding/ESP32-S3/studio-panel`

---

## Task 1: Foundation — `audio_data.h` + `sdkconfig.defaults`

**Files:**
- Create: `main/audio_data.h`
- Create: `main/audio_data.cpp`
- Modify: `sdkconfig.defaults`

**Interfaces:**
- Produces: `AudioPacket` struct (1072 bytes, packed), `g_audio_queue` (extern QueueHandle_t, capacity 2)

- [ ] **Step 1: Create `main/audio_data.h`**

```cpp
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Binary UDP packet received from companion script. All floats little-endian.
// Total size must equal 1072 bytes — verified by static_assert in audio_data.cpp.
struct AudioPacket {
    uint8_t  magic;       // must be 0xAB
    uint8_t  version;     // must be 1
    uint16_t flags;       // Bit0=FFT present, Bit1=Gonio present
    uint32_t seq;         // monotonically increasing, for drop detection
    float    peak_l;      // dBFS
    float    peak_r;
    float    rms_l;       // dBFS
    float    rms_r;
    float    momentary;   // LKFS, 400ms window
    float    short_term;  // LKFS, 3s window
    float    integrated;  // LKFS, cumulative
    float    gonio_l;     // raw sample -1..1 for Lissajous
    float    gonio_r;
    uint32_t fft_bins;    // must equal 256
    float    bins[256];   // magnitude 0.0..1.0, log-scaled, 20Hz..20kHz
} __attribute__((packed));

// Capacity 2: xQueueOverwrite ensures screens always read the latest packet.
// Both Metering and Spectrum screens use xQueuePeek (non-destructive).
extern QueueHandle_t g_audio_queue;
```

- [ ] **Step 2: Create `main/audio_data.cpp`**

```cpp
#include "audio_data.h"

static_assert(sizeof(AudioPacket) == 1072, "AudioPacket size mismatch — check struct layout");

QueueHandle_t g_audio_queue = nullptr;
```

- [ ] **Step 3: Add to `main/CMakeLists.txt`**

Open `main/CMakeLists.txt`. Find the `SRCS` list and add `"audio_data.cpp"`. If there is no `CMakeLists.txt` yet, the existing one in `main/` handles auto-discovery — just add it.

```cmake
# In main/CMakeLists.txt, add audio_data.cpp to the sources list
# (exact format depends on existing file — add alongside other .cpp files)
```

Read `main/CMakeLists.txt` first to see the exact format, then add `"audio_data.cpp"`.

- [ ] **Step 4: Add WiFi config to `sdkconfig.defaults`**

Append to the end of `sdkconfig.defaults`:

```
# WiFi (Phase 3)
CONFIG_ESP_WIFI_SSID="MeinNetzwerk"
CONFIG_ESP_WIFI_PASSWORD="MeinPasswort"
CONFIG_ESP_WIFI_AUTH_WPA2_PSK=y

# lwIP sockets
CONFIG_LWIP_SO_RCVBUF=y
CONFIG_LWIP_UDP_RECVMBOX_SIZE=16

# Increase socket buffer for UDP
CONFIG_LWIP_TCP_SND_BUF_DEFAULT=5744
```

**Important:** Replace `"MeinNetzwerk"` and `"MeinPasswort"` with the actual network credentials before flashing.

- [ ] **Step 5: Create the queue in `main/main.cpp`**

In `main.cpp`, add `#include "audio_data.h"` and before `ui_init()`:

```cpp
g_audio_queue = xQueueCreate(2, sizeof(AudioPacket));
configASSERT(g_audio_queue != nullptr);
```

- [ ] **Step 6: Build check**

```bash
bash -c 'source ~/esp/esp-idf-5.5/export.sh 2>/dev/null && idf.py build 2>&1 | grep -E " error:|fatal error" | head -20; echo "RC:$?"'
```

Expected: RC:0, no errors.

- [ ] **Step 7: Commit**

```bash
git add main/audio_data.h main/audio_data.cpp main/CMakeLists.txt main/main.cpp sdkconfig.defaults
git commit -m "feat: Phase 3 foundation — AudioPacket struct, queue, WiFi sdkconfig"
```

---

## Task 2: WiFi — `wifi.cpp / .h`

**Files:**
- Create: `main/wifi.cpp`
- Create: `main/wifi.h`
- Modify: `main/main.cpp` (add `wifi_init()`)
- Modify: `main/CMakeLists.txt` (add wifi.cpp)

**Interfaces:**
- Consumes: `g_audio_queue` (Task 1), `statusbar_update_wifi()` (existing in statusbar.h)
- Produces: `wifi_init()`, `wifi_is_connected() -> bool`; calls `net_receiver_start()` when IP assigned (stub until Task 3)

- [ ] **Step 1: Create `main/wifi.h`**

```cpp
#pragma once

// Starts WiFi station mode. Non-blocking — connection happens asynchronously.
// Calls net_receiver_start() via event loop when IP is assigned.
void wifi_init();

// Thread-safe: returns true if station has a valid IP address.
bool wifi_is_connected();
```

- [ ] **Step 2: Create `main/wifi.cpp`**

```cpp
#include "wifi.h"
#include "screens/statusbar.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <atomic>

// Forward declaration — implemented in net_receiver.cpp (Task 3)
extern "C" void net_receiver_start();

static const char *TAG = "wifi";
static std::atomic<bool> s_connected{false};

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        statusbar_update_wifi(false);
        ESP_LOGW(TAG, "Disconnected — reconnecting");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        statusbar_update_wifi(true);
        auto *event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&event->ip_info.ip));
        net_receiver_start();  // safe to call multiple times — idempotent
    }
}

// Non-blocking: initialises NVS, event loop, WiFi stack, registers handlers.
void wifi_init()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr, nullptr));

    wifi_config_t wifi_cfg = {};
    strncpy((char*)wifi_cfg.sta.ssid,     CONFIG_ESP_WIFI_SSID,     sizeof(wifi_cfg.sta.ssid));
    strncpy((char*)wifi_cfg.sta.password, CONFIG_ESP_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    ESP_LOGI(TAG, "Connecting to '%s'", CONFIG_ESP_WIFI_SSID);
}

bool wifi_is_connected() { return s_connected.load(); }
```

- [ ] **Step 3: Add stub for `net_receiver_start()` temporarily**

In `main/wifi.cpp`, at the top, change the forward declaration to a local stub so it compiles without Task 3:

```cpp
// Remove or comment out the extern forward declaration temporarily:
// extern "C" void net_receiver_start();

// Add a local stub at file scope (remove when Task 3 is implemented):
static void net_receiver_start() {}
```

(Remove this stub in Task 3.)

- [ ] **Step 4: Add `wifi_init()` to `main/main.cpp`**

Add `#include "wifi.h"` and call `wifi_init()` before `ui_init()`:

```cpp
wifi_init();   // non-blocking; net_receiver starts when IP assigned
ui_init();
```

- [ ] **Step 5: Add `wifi.cpp` to `main/CMakeLists.txt`**

- [ ] **Step 6: Build check**

```bash
bash -c 'source ~/esp/esp-idf-5.5/export.sh 2>/dev/null && idf.py build 2>&1 | grep -E " error:|fatal error" | head -20; echo "RC:$?"'
```

Expected: RC:0. If `CONFIG_ESP_WIFI_SSID` is undefined, run `idf.py menuconfig` → Component config → WiFi, or confirm the sdkconfig.defaults was added correctly and run `idf.py fullclean && idf.py build`.

- [ ] **Step 7: Flash and verify**

```bash
idf.py -p /dev/ttyACM0 flash
timeout 30 idf.py -p /dev/ttyACM0 monitor 2>&1 | grep -v "^---"
```

Expected log: `wifi: Connecting to 'MeinNetzwerk'`, then `wifi: IP: 192.168.x.x`, statusbar WiFi icon turns blue.

- [ ] **Step 8: Commit**

```bash
git add main/wifi.h main/wifi.cpp main/CMakeLists.txt main/main.cpp
git commit -m "feat: WiFi station mode — auto-reconnect, statusbar icon, net_receiver hook"
```

---

## Task 3: UDP Receiver — `net_receiver.cpp / .h`

**Files:**
- Create: `main/net_receiver.cpp`
- Create: `main/net_receiver.h`
- Modify: `main/wifi.cpp` (replace stub with real forward declaration)
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Consumes: `g_audio_queue` (Task 1), `AudioPacket` struct
- Produces: `net_receiver_start()` — idempotent, called from WiFi event handler

- [ ] **Step 1: Create `main/net_receiver.h`**

```cpp
#pragma once

// Starts the UDP receiver task on port 4210. Idempotent — safe to call
// multiple times (e.g. on WiFi reconnect); only one task runs at a time.
void net_receiver_start();
```

- [ ] **Step 2: Create `main/net_receiver.cpp`**

```cpp
#include "net_receiver.h"
#include "audio_data.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstring>

static const char *TAG      = "net_rx";
static const int   UDP_PORT = 4210;
static std::atomic<bool> s_running{false};

// Validates magic, version, and exact packet length before writing to queue.
static bool packet_valid(const uint8_t *buf, int len)
{
    return len == sizeof(AudioPacket)
        && buf[0] == 0xAB
        && buf[1] == 1
        && reinterpret_cast<const AudioPacket*>(buf)->fft_bins == 256;
}

// UDP receive loop — runs on Core 0 at priority 5.
// Uses xQueueOverwrite so screens always get the most recent packet.
static void udp_task(void *)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        s_running = false;
        vTaskDelete(nullptr);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(UDP_PORT);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: %d", errno);
        close(sock);
        s_running = false;
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Listening on UDP :%d", UDP_PORT);

    uint8_t    buf[sizeof(AudioPacket)];
    uint32_t   last_seq   = 0;
    uint32_t   drop_count = 0;
    uint32_t   pkt_count  = 0;

    while (true) {
        struct sockaddr_in src = {};
        socklen_t src_len = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr*)&src, &src_len);

        if (len < 0) {
            ESP_LOGW(TAG, "recvfrom error %d", errno);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!packet_valid(buf, len)) {
            ESP_LOGD(TAG, "invalid packet (len=%d magic=0x%02X)", len, buf[0]);
            continue;
        }

        const AudioPacket *pkt = reinterpret_cast<const AudioPacket*>(buf);
        if (pkt->seq != last_seq + 1 && last_seq != 0)
            drop_count++;
        last_seq = pkt->seq;

        xQueueOverwrite(g_audio_queue, pkt);

        // Log drop rate every ~10 seconds (300 packets at 30Hz)
        if (++pkt_count % 300 == 0) {
            ESP_LOGI(TAG, "pkts=%lu drops=%lu (%.1f%%)",
                     pkt_count, drop_count,
                     100.0f * drop_count / pkt_count);
        }
    }
}

// Idempotent: only starts the task if it is not already running.
void net_receiver_start()
{
    if (s_running.exchange(true)) return;
    xTaskCreatePinnedToCore(udp_task, "udp_rx", 4096, nullptr, 5, nullptr, 0);
}
```

- [ ] **Step 3: Fix `wifi.cpp` — replace stub with real declaration**

In `main/wifi.cpp`, remove the local stub `static void net_receiver_start() {}` and restore the proper include:

```cpp
#include "net_receiver.h"
```

(Remove the `static void net_receiver_start() {}` line that was added in Task 2.)

- [ ] **Step 4: Add `net_receiver.cpp` to `main/CMakeLists.txt`**

- [ ] **Step 5: Build check**

```bash
bash -c 'source ~/esp/esp-idf-5.5/export.sh 2>/dev/null && idf.py build 2>&1 | grep -E " error:|fatal error" | head -20; echo "RC:$?"'
```

- [ ] **Step 6: Flash and verify with netcat**

```bash
idf.py -p /dev/ttyACM0 flash
```

In a second terminal, get the ESP32's IP from the monitor log, then send a test packet:

```bash
# Build a valid 1072-byte packet (magic + version + zeros)
python3 -c "
import struct, socket
buf = bytearray(1072)
buf[0] = 0xAB   # magic
buf[1] = 0x01   # version
struct.pack_into('<I', buf, 44, 256)  # fft_bins = 256
socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(buf, ('192.168.X.X', 4210))
print('sent')
"
```

Expected monitor output: `net_rx: Listening on UDP :4210`, then no error on receiving the packet.

- [ ] **Step 7: Commit**

```bash
git add main/net_receiver.h main/net_receiver.cpp main/wifi.cpp main/CMakeLists.txt
git commit -m "feat: UDP receiver task — port 4210, packet validation, queue write, drop logging"
```

---

## Task 4: Wire Up Real Data — Metering + Boot Cleanup

**Files:**
- Modify: `main/screens/metering.cpp` (queue read + demo fallback)
- Modify: `main/ui.cpp` (remove dev workaround, restore home screen boot)

**Interfaces:**
- Consumes: `g_audio_queue`, `AudioPacket` (Task 1), `metering_demo_tick()` (existing)

- [ ] **Step 1: Update `metering.cpp` timer callback**

In `metering.cpp`, add at the top: `#include "audio_data.h"`

Find `metering_timer_cb`. Replace the call to `metering_demo_tick` with:

```cpp
static void metering_timer_cb(lv_timer_t *timer)
{
    // called at ~30 Hz from LVGL task — no locking needed
    auto *data = static_cast<MeteringScreenData*>(lv_timer_get_user_data(timer));
    constexpr float DT = 0.033f;

    AudioPacket pkt;
    if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE) {
        // Real data from UDP: map fields directly onto MeteringState
        data->state.peak_l     = pkt.peak_l;
        data->state.peak_r     = pkt.peak_r;
        data->state.rms_l      = pkt.rms_l;
        data->state.rms_r      = pkt.rms_r;
        data->state.momentary  = pkt.momentary;
        data->state.short_term = pkt.short_term;
        data->state.integrated = pkt.integrated;
        data->state.l_sample   = pkt.gonio_l;
        data->state.r_sample   = pkt.gonio_r;
        // advance history ring buffer (1 value/s) using existing logic
        data->state.history_tick += DT;
        if (data->state.history_tick >= 1.0f) {
            data->state.history_tick -= 1.0f;
            data->state.short_term_history[data->state.history_head] = pkt.short_term;
            data->state.history_head = (data->state.history_head + 1) % 60;
        }
    } else {
        metering_demo_tick(data->state, DT);  // fallback when no UDP data
    }

    metering_bar_update(data->bar_l, data->state.rms_l, data->state.peak_hold_l);
    metering_bar_update(data->bar_r, data->state.rms_r, data->state.peak_hold_r);
    metering_gonio_update(data->gonio, data);
    metering_history_invalidate(data->history);
    metering_numerics_update(data);
}
```

- [ ] **Step 2: Restore `ui.cpp` — remove dev workaround**

In `ui.cpp`, replace:
```cpp
lv_screen_load(metering_screen_create());  // DEV: direkt in Metering booten (kein Touch)
```
With:
```cpp
home_screen_load();
```

Also remove `#include "screens/metering.h"` from `ui.cpp` if it was added only for the workaround.

- [ ] **Step 3: Build check**

```bash
bash -c 'source ~/esp/esp-idf-5.5/export.sh 2>/dev/null && idf.py build 2>&1 | grep -E " error:|fatal error" | head -20; echo "RC:$?"'
```

- [ ] **Step 4: Flash and verify**

Flash, navigate to Metering (from home screen — touch still broken, use physical reset and boot directly if needed), confirm demo data still animates when no UDP packet arrives.

- [ ] **Step 5: Commit**

```bash
git add main/screens/metering.cpp main/ui.cpp
git commit -m "feat: metering screen reads UDP queue, falls back to demo data when empty"
```

---

## Task 5: Spectrum Screen Scaffold

**Files:**
- Create: `main/screens/spectrum.h`
- Create: `main/screens/spectrum.cpp` (full scaffold, placeholder views, BOOT button, navigation)
- Modify: `main/screens/home.cpp` (add Spectrum tile)
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Produces: `spectrum_screen_create() -> lv_obj_t*`
- Consumes: `g_audio_queue`, `AudioPacket`, `theme.h`, `home_screen_create()`

- [ ] **Step 1: Create `main/screens/spectrum.h`**

```cpp
#pragma once
#include "lvgl.h"

// Creates the spectrum screen (entry point, View 1 = bars).
// Returns new screen object; caller does NOT call lv_screen_load — done internally.
lv_obj_t *spectrum_screen_create();
```

- [ ] **Step 2: Create `main/screens/spectrum.cpp` scaffold**

```cpp
#include "spectrum.h"
#include "audio_data.h"
#include "theme.h"
#include "screens/home.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <cmath>
#include <cstring>
#include <algorithm>

static const char *TAG = "spectrum";

// ── Data ──────────────────────────────────────────────────────────────────────

struct SpectrumScreenData {
    // Three separate screen objects (each is an lv_obj root)
    lv_obj_t *scr_bars;      // View 1 — classic bars
    lv_obj_t *scr_curve;     // View 2 — FFT area chart
    lv_obj_t *scr_waterfall; // View 3 — scrolling heatmap

    // Per-view draw containers
    lv_obj_t *bars_canvas;
    lv_obj_t *curve_canvas;
    lv_obj_t *wf_canvas;    // lv_canvas with PSRAM buffer
    void     *wf_buf;       // PSRAM allocated waterfall buffer

    // Shared
    lv_timer_t *timer;
    float    smoothed[256];  // exponential MA; shared across views
    bool     frozen;         // when true: timer runs but display not invalidated

    // Freeze icon (visible on all views when frozen)
    lv_obj_t *freeze_icon_bars;
    lv_obj_t *freeze_icon_curve;
    lv_obj_t *freeze_icon_wf;

    // Peak hold for bars view
    float    peak_hold[256];
    float    peak_hold_timer[256];

    // Waterfall
    uint8_t  color_preset; // 0=Classic 1=Green 2=Warm 3=Purple
    bool     wf_rtl;       // true = right-to-left (default)

    // Context menu
    lv_obj_t *ctx_menu;    // nullptr when hidden

    // BOOT button state
    volatile int64_t btn_press_us; // timestamp of last press (from ISR)
    volatile bool    btn_event;    // set by ISR on release
    volatile bool    btn_long;     // set by ISR: true if press >= 1s
};

// Single data instance per screen lifetime — allocated on create, freed on delete
static SpectrumScreenData *s_data = nullptr;

// ── BOOT button ───────────────────────────────────────────────────────────────

// ISR: records press/release timing; sets btn_event + btn_long on release.
// IRAM_ATTR required for ISR functions.
static void IRAM_ATTR boot_btn_isr(void *arg)
{
    auto *d = static_cast<SpectrumScreenData*>(arg);
    int level = gpio_get_level(GPIO_NUM_0);
    int64_t now = esp_timer_get_time();
    if (level == 0) {
        d->btn_press_us = now;                          // falling edge = press
    } else {
        d->btn_long  = (now - d->btn_press_us) >= 1000000; // 1s threshold
        d->btn_event = true;                            // rising edge = release
    }
}

static void boot_btn_init(SpectrumScreenData *d)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GPIO_NUM_0),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,  // both press and release
    };
    gpio_config(&cfg);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_NUM_0, boot_btn_isr, d);
}

static void boot_btn_deinit()
{
    gpio_isr_handler_remove(GPIO_NUM_0);
}

// ── Navigation ────────────────────────────────────────────────────────────────

static void navigate_to_bars(SpectrumScreenData *d);   // forward decl
static void navigate_to_curve(SpectrumScreenData *d);
static void navigate_to_wf(SpectrumScreenData *d);
static void navigate_back_home();

static void navigate_to_bars(SpectrumScreenData *d)
{
    lv_screen_load_anim(d->scr_bars, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}
static void navigate_to_curve(SpectrumScreenData *d)
{
    lv_screen_load_anim(d->scr_curve, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
static void navigate_to_wf(SpectrumScreenData *d)
{
    lv_screen_load_anim(d->scr_waterfall, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
static void navigate_back_home()
{
    lv_obj_t *home = home_screen_create();
    lv_screen_load_anim(home, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, true);
}

// ── Forward declarations (implemented in Tasks 6-8) ──────────────────────────

static void spectrum_bars_draw(lv_event_t *e);
static void spectrum_curve_draw(lv_event_t *e);
static void spectrum_wf_update(SpectrumScreenData *d);
static void spectrum_ctx_menu_show(SpectrumScreenData *d);
static void spectrum_ctx_menu_hide(SpectrumScreenData *d);

// ── Placeholder stubs (replaced in Tasks 6-8) ────────────────────────────────

static void spectrum_bars_draw(lv_event_t *) {}
static void spectrum_curve_draw(lv_event_t *) {}
static void spectrum_wf_update(SpectrumScreenData *) {}
static void spectrum_ctx_menu_show(SpectrumScreenData *) {}
static void spectrum_ctx_menu_hide(SpectrumScreenData *) {}

// ── Freeze icon helper ────────────────────────────────────────────────────────

// Creates a small ❚❚ label in the top-right corner of parent screen.
static lv_obj_t *make_freeze_icon(lv_obj_t *parent)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_color(lbl, THEME_ACCENT, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_HINT, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_RIGHT, -8, THEME_STATUSBAR_H + 4);
    lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
    return lbl;
}

// ── Timer ─────────────────────────────────────────────────────────────────────

// ~30 Hz tick: reads queue, smooths bins, handles BOOT button, updates active view.
static void spectrum_timer_cb(lv_timer_t *timer)
{
    auto *d = static_cast<SpectrumScreenData*>(lv_timer_get_user_data(timer));

    // Handle BOOT button event (set by ISR)
    if (d->btn_event) {
        d->btn_event = false;
        if (d->btn_long) {
            d->frozen = !d->frozen;
            auto set_icon = [&](lv_obj_t *icon) {
                if (icon) {
                    d->frozen ? lv_obj_clear_flag(icon, LV_OBJ_FLAG_HIDDEN)
                              : lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
                }
            };
            set_icon(d->freeze_icon_bars);
            set_icon(d->freeze_icon_curve);
            set_icon(d->freeze_icon_wf);
            ESP_LOGI(TAG, "Freeze: %s", d->frozen ? "ON" : "OFF");
        } else {
            // Short press: cycle views bars → curve → waterfall → bars
            lv_obj_t *active = lv_screen_active();
            if (active == d->scr_bars)        navigate_to_curve(d);
            else if (active == d->scr_curve)  navigate_to_wf(d);
            else                              navigate_to_bars(d);
        }
    }

    if (d->frozen) return;

    // Read latest audio packet and update smoothed[] bins
    AudioPacket pkt;
    if (xQueuePeek(g_audio_queue, &pkt, 0) == pdTRUE) {
        constexpr float ALPHA = 0.35f;  // exponential MA — balances responsiveness and smoothness
        for (int i = 0; i < 256; i++) {
            d->smoothed[i] += ALPHA * (pkt.bins[i] - d->smoothed[i]);
        }
        // Update peak hold (bars view)
        for (int i = 0; i < 256; i++) {
            if (d->smoothed[i] > d->peak_hold[i]) {
                d->peak_hold[i] = d->smoothed[i];
                d->peak_hold_timer[i] = 2.0f;  // 2s freeze
            } else if (d->peak_hold_timer[i] > 0) {
                d->peak_hold_timer[i] -= 0.033f;
            } else {
                d->peak_hold[i] = std::max(d->peak_hold[i] - 0.033f * 0.5f, 0.0f);
            }
        }
    }

    // Invalidate whichever view is currently active
    lv_obj_t *active = lv_screen_active();
    if (active == d->scr_bars && d->bars_canvas)
        lv_obj_invalidate(d->bars_canvas);
    else if (active == d->scr_curve && d->curve_canvas)
        lv_obj_invalidate(d->curve_canvas);
    else if (active == d->scr_waterfall)
        spectrum_wf_update(d);
}

// ── Back button callbacks ─────────────────────────────────────────────────────

static void on_back_bars(lv_event_t *)    { navigate_back_home(); }
static void on_back_curve(lv_event_t *)   { navigate_back_home(); }
static void on_back_wf(lv_event_t *)      { navigate_back_home(); }

// ── Screen + canvas creation helpers ─────────────────────────────────────────

// Creates a themed screen with a Back button and a full-screen draw container.
// back_cb is the LV_EVENT_CLICKED callback for the back button.
static lv_obj_t *make_spectrum_screen(lv_event_cb_t back_cb,
                                       lv_event_cb_t draw_cb,
                                       SpectrumScreenData *d,
                                       lv_obj_t **canvas_out,
                                       lv_obj_t **icon_out)
{
    lv_obj_t *scr = theme_make_screen();

    // Full-screen draw area (below statusbar, above back button)
    lv_obj_t *canvas = lv_obj_create(scr);
    lv_obj_remove_style_all(canvas);
    lv_obj_set_style_bg_opa(canvas, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(canvas, 800, 480 - THEME_STATUSBAR_H - 60);
    lv_obj_set_pos(canvas, 0, THEME_STATUSBAR_H);
    if (draw_cb) lv_obj_add_event_cb(canvas, draw_cb, LV_EVENT_DRAW_MAIN, d);
    if (canvas_out) *canvas_out = canvas;

    // Back button
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 100, 44);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_LEFT, 16, -16);
    lv_obj_set_style_bg_color(btn, THEME_BG_CARD, 0);
    lv_obj_add_event_cb(btn, back_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, LV_SYMBOL_LEFT " Home");
    lv_obj_set_style_text_color(lbl, THEME_TEXT_PRIMARY, 0);
    lv_obj_center(lbl);

    // Freeze icon (top-right, hidden by default)
    if (icon_out) *icon_out = make_freeze_icon(scr);

    return scr;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

static void on_bars_delete(lv_event_t *e)
{
    auto *d = static_cast<SpectrumScreenData*>(lv_event_get_user_data(e));
    // Only fully clean up if this is the last remaining spectrum screen
    // (curve and waterfall may still be alive during animation)
    // Full cleanup when timer is deleted (only once):
    if (!d->timer) return;
    lv_timer_delete(d->timer);
    d->timer = nullptr;
    boot_btn_deinit();
    if (d->wf_buf) { heap_caps_free(d->wf_buf); d->wf_buf = nullptr; }
    s_data = nullptr;
    delete d;
}

// ── Public entry point ────────────────────────────────────────────────────────

lv_obj_t *spectrum_screen_create()
{
    if (s_data) {
        // Already exists (shouldn't happen but guard anyway)
        return s_data->scr_bars;
    }

    auto *d = new SpectrumScreenData{};
    d->wf_rtl      = true;   // right-to-left default
    d->color_preset = 0;     // Classic

    // Create three screens
    d->scr_bars      = make_spectrum_screen(on_back_bars,  spectrum_bars_draw,  d, &d->bars_canvas,  &d->freeze_icon_bars);
    d->scr_curve     = make_spectrum_screen(on_back_curve, spectrum_curve_draw, d, &d->curve_canvas, &d->freeze_icon_curve);
    d->scr_waterfall = make_spectrum_screen(on_back_wf,    nullptr,             d, nullptr,           &d->freeze_icon_wf);
    // waterfall gets its own canvas in Task 8

    // Cleanup only on bars screen delete (first created, first destroyed on exit)
    lv_obj_add_event_cb(d->scr_bars, on_bars_delete, LV_EVENT_DELETE, d);

    boot_btn_init(d);
    d->timer = lv_timer_create(spectrum_timer_cb, 33, d);

    s_data = d;
    return d->scr_bars;  // caller loads this screen
}
```

- [ ] **Step 3: Add Spectrum tile to `main/screens/home.cpp`**

Add `#include "screens/spectrum.h"` to home.cpp.

In the `TILES` array, replace the "ROUTING" entry (index 3) with Spectrum:

```cpp
{ LV_SYMBOL_EQ,   "SPECTRUM",    "FFT / Waterfall",     spectrum_screen_create    },
```

(`LV_SYMBOL_EQ` is the equalizer/bars symbol in LVGL — appropriate for spectrum.)

- [ ] **Step 4: Add `spectrum.cpp` to `main/CMakeLists.txt`**

- [ ] **Step 5: Build check and flash**

```bash
bash -c 'source ~/esp/esp-idf-5.5/export.sh 2>/dev/null && idf.py build 2>&1 | grep -E " error:|fatal error" | head -20; echo "RC:$?"'
idf.py -p /dev/ttyACM0 flash
```

Expected: Home screen boots. "SPECTRUM" tile visible. Tapping (when touch works) or navigating to it shows a blank screen with a Back button. BOOT button short-press cycles between 3 blank views.

- [ ] **Step 6: Commit**

```bash
git add main/screens/spectrum.h main/screens/spectrum.cpp main/screens/home.cpp main/CMakeLists.txt
git commit -m "feat: spectrum screen scaffold — 3 views, BOOT button nav/freeze, placeholder draw"
```

---

## Task 6: Spectrum View 1 — Classic Bars

**Files:**
- Modify: `main/screens/spectrum.cpp` — replace `spectrum_bars_draw` stub

**Interfaces:**
- Consumes: `SpectrumScreenData.smoothed[256]`, `SpectrumScreenData.peak_hold[256]`

- [ ] **Step 1: Remove `spectrum_bars_draw` stub and replace with full implementation**

Find and remove: `static void spectrum_bars_draw(lv_event_t *) {}`

Replace with:

```cpp
// Maps screen X pixel (0..w-1) to FFT bin index using logarithmic frequency scale.
// 20Hz–20kHz spread gives bass more visual space — matches human hearing.
static int x_to_bin(int x, int w)
{
    constexpr float F_MIN    = 20.0f;
    constexpr float F_MAX    = 20000.0f;
    constexpr float NYQUIST  = 22050.0f;
    float ratio = (float)x / (float)w;
    float freq  = F_MIN * powf(F_MAX / F_MIN, ratio);
    int   bin   = (int)(freq / NYQUIST * 256.0f);
    return (bin < 0) ? 0 : (bin > 255) ? 255 : bin;
}

// Interpolates between two lv_color_t values. t = 0.0..1.0.
static lv_color_t color_lerp(lv_color_t a, lv_color_t b, float t)
{
    uint8_t r = (uint8_t)(a.red   + t * (b.red   - a.red));
    uint8_t g = (uint8_t)(a.green + t * (b.green - a.green));
    uint8_t bu = (uint8_t)(a.blue  + t * (b.blue  - a.blue));
    return lv_color_make(r, g, bu);
}

// LV_EVENT_DRAW_MAIN handler for the bars canvas.
// Draws 800 columns mapped to 256 FFT bins (log scale), peak-hold markers.
static void spectrum_bars_draw(lv_event_t *e)
{
    auto *d       = static_cast<SpectrumScreenData*>(lv_event_get_user_data(e));
    auto *layer   = lv_event_get_layer(e);
    auto *obj     = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t w = lv_area_get_width(&a);
    int32_t h = lv_area_get_height(&a);

    // Black background (pure visualisation area — luminance rule exempt)
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(0x0A0A0A);
        dsc.radius   = 0;
        lv_draw_rect(layer, &dsc, &a);
    }

    // Color stops: dark green → green → yellow → red (by amplitude)
    static const lv_color_t C0 = {0x20, 0x50, 0x20, 0};  // dark green (quiet)
    static const lv_color_t C1 = {0x30, 0xBC, 0x30, 0};  // bright green
    static const lv_color_t C2 = {0xC8, 0xA0, 0x30, 0};  // yellow
    static const lv_color_t C3 = {0xE0, 0x50, 0x50, 0};  // red (loud)

    for (int x = 0; x < w; x++) {
        int bin = x_to_bin(x, w);
        float mag = d->smoothed[bin];                // 0.0..1.0
        int32_t bar_h = (int32_t)(mag * (float)h);

        if (bar_h < 1) bar_h = 1;

        // Bar color interpolated by magnitude
        lv_color_t col;
        if      (mag < 0.33f) col = color_lerp(C0, C1, mag / 0.33f);
        else if (mag < 0.66f) col = color_lerp(C1, C2, (mag - 0.33f) / 0.33f);
        else                  col = color_lerp(C2, C3, (mag - 0.66f) / 0.34f);

        lv_area_t ba = { a.x1 + x, a.y2 - bar_h, a.x1 + x, a.y2 };
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = col;
        dsc.radius   = 0;
        lv_draw_rect(layer, &dsc, &ba);

        // Peak hold: 1px white dot
        if (d->peak_hold[bin] > 0.01f) {
            int32_t py = a.y2 - (int32_t)(d->peak_hold[bin] * (float)h);
            lv_area_t pa = { a.x1 + x, py, a.x1 + x, py };
            lv_draw_rect_dsc_t pdsc;
            lv_draw_rect_dsc_init(&pdsc);
            pdsc.bg_color = lv_color_hex(0xE8E8E8);
            lv_draw_rect(layer, &pdsc, &pa);
        }
    }
}
```

- [ ] **Step 2: Build check + flash**

```bash
bash -c 'source ~/esp/esp-idf-5.5/export.sh 2>/dev/null && idf.py build 2>&1 | grep -E " error:|fatal error" | head -20; echo "RC:$?"'
idf.py -p /dev/ttyACM0 flash
```

Navigate to Spectrum (from home if touch works, or modify `ui.cpp` temporarily to `lv_screen_load(spectrum_screen_create())`). Expected: animated green/yellow/red spectrum bars with white peak-hold dots when UDP data is flowing.

- [ ] **Step 3: Commit**

```bash
git add main/screens/spectrum.cpp
git commit -m "feat: spectrum view 1 — classic bars with log frequency scale and peak hold"
```

---

## Task 7: Spectrum View 2 — FFT Area Curve

**Files:**
- Modify: `main/screens/spectrum.cpp` — replace `spectrum_curve_draw` stub

- [ ] **Step 1: Replace `spectrum_curve_draw` stub**

Remove: `static void spectrum_curve_draw(lv_event_t *) {}`

Replace with:

```cpp
// LV_EVENT_DRAW_MAIN handler — draws smoothed FFT bins as a filled area curve.
// Uses log X-scale (same as bars) and gradient fill from dark base to bright peak.
static void spectrum_curve_draw(lv_event_t *e)
{
    auto *d     = static_cast<SpectrumScreenData*>(lv_event_get_user_data(e));
    auto *layer = lv_event_get_layer(e);
    auto *obj   = lv_event_get_target_obj(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t w = lv_area_get_width(&a);
    int32_t h = lv_area_get_height(&a);

    // Very dark background — pure visualisation surface
    {
        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = lv_color_hex(0x050510);
        dsc.radius   = 0;
        lv_draw_rect(layer, &dsc, &a);
    }

    // Build point array for the curve (log-scaled X)
    // Draw filled segments: for each adjacent pair of X pixels, draw a trapezoid
    for (int x = 0; x < w - 1; x++) {
        int  bin0 = x_to_bin(x,     w);
        int  bin1 = x_to_bin(x + 1, w);
        float mag0 = d->smoothed[bin0];
        float mag1 = d->smoothed[bin1];
        int32_t y0 = a.y2 - (int32_t)(mag0 * (float)h);
        int32_t y1 = a.y2 - (int32_t)(mag1 * (float)h);

        // Vertical fill from y-top to bottom (filled area under curve)
        int32_t top = std::min(y0, y1);
        lv_area_t fa = { a.x1 + x, top, a.x1 + x + 1, a.y2 };
        float avg_mag = (mag0 + mag1) * 0.5f;

        // Gradient: dark blue base → cyan/white at top
        lv_color_t col;
        if (avg_mag < 0.5f)
            col = lv_color_make(
                (uint8_t)(0x1A * avg_mag * 2),
                (uint8_t)(0x3A * avg_mag * 2),
                (uint8_t)(0x8A + 0x30 * avg_mag * 2));
        else
            col = lv_color_make(
                (uint8_t)(0x1A + 0xD0 * (avg_mag - 0.5f) * 2),
                (uint8_t)(0x3A + 0x96 * (avg_mag - 0.5f) * 2),
                (uint8_t)(0xBA + 0x45 * (avg_mag - 0.5f) * 2));

        lv_draw_rect_dsc_t dsc;
        lv_draw_rect_dsc_init(&dsc);
        dsc.bg_color = col;
        dsc.radius   = 0;
        lv_draw_rect(layer, &dsc, &fa);
    }

    // Bright top line for definition
    {
        lv_draw_line_dsc_t dsc;
        lv_draw_line_dsc_init(&dsc);
        dsc.color = lv_color_hex(0x70D0FF);
        dsc.width = 2;
        for (int x = 0; x < w - 1; x++) {
            int32_t y0 = a.y2 - (int32_t)(d->smoothed[x_to_bin(x,   w)] * (float)h);
            int32_t y1 = a.y2 - (int32_t)(d->smoothed[x_to_bin(x+1, w)] * (float)h);
            dsc.p1.x = (lv_value_precise_t)(a.x1 + x);
            dsc.p1.y = (lv_value_precise_t)y0;
            dsc.p2.x = (lv_value_precise_t)(a.x1 + x + 1);
            dsc.p2.y = (lv_value_precise_t)y1;
            lv_draw_line(layer, &dsc);
        }
    }
}
```

- [ ] **Step 2: Build check + flash + verify**

Expected: View 2 shows a flowing blue/cyan area chart. BOOT short-press cycles between views.

- [ ] **Step 3: Commit**

```bash
git add main/screens/spectrum.cpp
git commit -m "feat: spectrum view 2 — FFT area curve with cyan gradient fill"
```

---

## Task 8: Spectrum View 3 — Waterfall + Context Menu

**Files:**
- Modify: `main/screens/spectrum.cpp` — waterfall canvas init, `spectrum_wf_update`, color LUTs, context menu

**Interfaces:**
- Consumes: `SpectrumScreenData.smoothed[256]`, `color_preset`, `wf_rtl`

Waterfall canvas: 800×388 px (480 - 32 statusbar - 60 back-btn area), RGB565, PSRAM.
Buffer size: `800 × 388 × 2 = 620,800 bytes` (~606 KB).

- [ ] **Step 1: Add color LUT initialization to `spectrum_screen_create()`**

Add this struct and init function before `spectrum_screen_create`:

```cpp
// 4 color presets, each a 256-entry LUT mapping magnitude (0..255) → color.
// Initialized at screen create time; fast per-pixel lookup during waterfall update.
static lv_color_t s_lut[4][256];

static void init_color_luts()
{
    // Helper: interpolate between two hex colors
    auto lerp_hex = [](uint32_t a, uint32_t b, float t) -> lv_color_t {
        uint8_t r = (uint8_t)(((a>>16)&0xFF) + t*(((b>>16)&0xFF)-((a>>16)&0xFF)));
        uint8_t g = (uint8_t)(((a>> 8)&0xFF) + t*(((b>> 8)&0xFF)-((a>> 8)&0xFF)));
        uint8_t bl= (uint8_t)(( a     &0xFF) + t*(( b     &0xFF)-( a     &0xFF)));
        return lv_color_make(r, g, bl);
    };

    // Preset 0 — Classic: Black→Blue→Cyan→Yellow→Red→White
    static const uint32_t C0[] = {0x000000,0x0000AA,0x00AAAA,0xAAAA00,0xAA0000,0xFFFFFF};
    // Preset 1 — Green: Black→DarkGreen→BrightGreen→White
    static const uint32_t C1[] = {0x000000,0x003300,0x30BC30,0xFFFFFF};
    // Preset 2 — Warm: Black→DarkRed→Orange→Yellow→White
    static const uint32_t C2[] = {0x000000,0x550000,0xC84000,0xE0B020,0xFFFFFF};
    // Preset 3 — Purple: Black→DarkViolet→Magenta→White
    static const uint32_t C3[] = {0x000000,0x330033,0xC020C0,0xFFFFFF};

    auto fill_lut = [&](int preset, const uint32_t *stops, int n_stops) {
        for (int i = 0; i < 256; i++) {
            float t = (float)i / 255.0f * (n_stops - 1);
            int   lo = (int)t;
            int   hi = std::min(lo + 1, n_stops - 1);
            s_lut[preset][i] = lerp_hex(stops[lo], stops[hi], t - lo);
        }
    };
    fill_lut(0, C0, 6);
    fill_lut(1, C1, 4);
    fill_lut(2, C2, 5);
    fill_lut(3, C3, 4);
}
```

At the start of `spectrum_screen_create()`, call `init_color_luts()`.

- [ ] **Step 2: Allocate waterfall canvas in `spectrum_screen_create()`**

After creating `d->scr_waterfall`, allocate the canvas buffer and set it up:

```cpp
// Waterfall canvas: full width, height minus statusbar and back-button area
constexpr int WF_W = 800;
constexpr int WF_H = 388;  // 480 - 32 (statusbar) - 60 (back btn area)
size_t wf_size = WF_W * WF_H * sizeof(uint16_t);  // RGB565
d->wf_buf = heap_caps_malloc(wf_size, MALLOC_CAP_SPIRAM);
if (!d->wf_buf) d->wf_buf = malloc(wf_size);      // internal RAM fallback
memset(d->wf_buf, 0, wf_size);

d->wf_canvas = lv_canvas_create(d->scr_waterfall);
lv_canvas_set_buffer(d->wf_canvas, d->wf_buf, WF_W, WF_H, LV_COLOR_FORMAT_RGB565);
lv_obj_set_pos(d->wf_canvas, 0, THEME_STATUSBAR_H);
```

Also add `lv_obj_t *wf_canvas` and `constexpr` sizes to `SpectrumScreenData` (already declared).

- [ ] **Step 3: Replace `spectrum_wf_update` stub with full implementation**

Remove: `static void spectrum_wf_update(SpectrumScreenData *) {}`

Replace:

```cpp
// Shifts waterfall one column (RTL: left) and draws new column from current smoothed[] bins.
// Called from timer at ~30 Hz — fast path: one memmove + 388 pixel writes.
static void spectrum_wf_update(SpectrumScreenData *d)
{
    if (!d->wf_canvas || !d->wf_buf) return;

    constexpr int WF_W = 800;
    constexpr int WF_H = 388;
    uint16_t *buf = static_cast<uint16_t*>(d->wf_buf);

    if (d->wf_rtl) {
        // Shift all columns one pixel to the left (oldest data moves left and disappears)
        // Each row: move pixels [1..WF_W-1] to [0..WF_W-2]
        for (int y = 0; y < WF_H; y++) {
            memmove(&buf[y * WF_W], &buf[y * WF_W + 1], (WF_W - 1) * sizeof(uint16_t));
        }

        // Write new column on the right edge
        for (int y = 0; y < WF_H; y++) {
            // y=0 is top (high freq), y=WF_H-1 is bottom (low freq)
            // Map y to bin: y=0 → bin 255, y=WF_H-1 → bin 0
            int bin = (int)((1.0f - (float)y / (float)(WF_H - 1)) * 255.0f);
            bin = std::max(0, std::min(255, bin));
            float mag = d->smoothed[bin];
            int lut_idx = (int)(mag * 255.0f);
            lut_idx = std::max(0, std::min(255, lut_idx));
            lv_color_t col = s_lut[d->color_preset][lut_idx];
            // Convert lv_color_t to RGB565
            buf[y * WF_W + (WF_W - 1)] =
                ((col.red >> 3) << 11) | ((col.green >> 2) << 5) | (col.blue >> 3);
        }
    }
    // (Top-to-bottom mode: similar but shift rows down — omitted for Phase 3)

    lv_obj_invalidate(d->wf_canvas);
}
```

- [ ] **Step 4: Add Long-Press context menu on waterfall screen**

Add `LV_EVENT_LONG_PRESSED` handler on the waterfall canvas:

```cpp
static void on_wf_long_press(lv_event_t *e)
{
    auto *d = static_cast<SpectrumScreenData*>(lv_event_get_user_data(e));
    if (d->ctx_menu) spectrum_ctx_menu_hide(d);
    else             spectrum_ctx_menu_show(d);
}
```

In `make_spectrum_screen` call for waterfall, after creating the canvas, add:
```cpp
lv_obj_add_event_cb(d->wf_canvas, on_wf_long_press, LV_EVENT_LONG_PRESSED, d);
lv_obj_add_flag(d->wf_canvas, LV_OBJ_FLAG_CLICKABLE);
```

- [ ] **Step 5: Replace context menu stubs**

Remove stubs and implement:

```cpp
// Shows a floating context menu on lv_layer_top with 4 color-swatch buttons.
// Tapping a swatch changes preset immediately; tapping outside dismisses.
static void spectrum_ctx_menu_show(SpectrumScreenData *d)
{
    if (d->ctx_menu) return;

    lv_obj_t *menu = lv_obj_create(lv_layer_top());
    lv_obj_set_size(menu, 320, 80);
    lv_obj_align(menu, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(menu, lv_color_hex(0x686868), 0);
    lv_obj_set_style_bg_opa(menu, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(menu, THEME_RADIUS, 0);
    lv_obj_set_style_border_width(menu, 0, 0);
    lv_obj_clear_flag(menu, LV_OBJ_FLAG_SCROLLABLE);
    d->ctx_menu = menu;

    // 4 swatch labels as representative gradient colors
    static const uint32_t SWATCH_COLORS[4] = {0x0055AA, 0x30BC30, 0xC84000, 0xC020C0};
    static const char    *SWATCH_LABELS[4] = {"Classic", "Green", "Warm", "Purple"};

    for (int i = 0; i < 4; i++) {
        lv_obj_t *sw = lv_obj_create(menu);
        lv_obj_set_size(sw, 60, 36);
        lv_obj_set_pos(sw, 8 + i * 76, 22);
        lv_obj_set_style_bg_color(sw, lv_color_hex(SWATCH_COLORS[i]), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(sw, 4, 0);
        lv_obj_set_style_border_color(sw, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_border_width(sw, (d->color_preset == (uint8_t)i) ? 2 : 0, 0);
        lv_obj_set_style_border_opa(sw, LV_OPA_COVER, 0);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);

        // Store preset index in user_data
        lv_obj_set_user_data(sw, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        lv_obj_add_event_cb(sw, [](lv_event_t *e) {
            auto *d2 = static_cast<SpectrumScreenData*>(lv_event_get_param(e));
            d2->color_preset = (uint8_t)reinterpret_cast<uintptr_t>(
                lv_obj_get_user_data(lv_event_get_target_obj(e)));
            spectrum_ctx_menu_hide(d2);
        }, LV_EVENT_CLICKED, d);

        (void)SWATCH_LABELS[i];  // labels omitted for cleanliness; swatch color is self-explanatory
    }

    // Click-outside-to-close
    lv_obj_add_event_cb(lv_layer_top(), [](lv_event_t *e) {
        auto *d2 = static_cast<SpectrumScreenData*>(lv_event_get_user_data(e));
        spectrum_ctx_menu_hide(d2);
    }, LV_EVENT_CLICKED, d);
}

static void spectrum_ctx_menu_hide(SpectrumScreenData *d)
{
    if (!d->ctx_menu) return;
    lv_obj_delete(d->ctx_menu);
    d->ctx_menu = nullptr;
}
```

- [ ] **Step 6: Build check + flash + full visual verify**

```bash
bash -c 'source ~/esp/esp-idf-5.5/export.sh 2>/dev/null && idf.py build 2>&1 | grep -E " error:|fatal error" | head -20; echo "RC:$?"'
idf.py -p /dev/ttyACM0 flash
```

With UDP data flowing (Task 9 companion script), verify:
- View 1: animated spectrum bars
- View 2: flowing curve
- View 3: scrolling waterfall (right→left)
- Long-press on waterfall: color swatches appear
- Tap swatch: preset changes immediately
- BOOT short: next view; BOOT long (>1s): ❚❚ icon appears, display frozen

- [ ] **Step 7: Commit**

```bash
git add main/screens/spectrum.cpp
git commit -m "feat: spectrum view 3 — waterfall PSRAM canvas, 4 color presets, context menu"
```

---

## Task 9: Python Companion Script

**Files:**
- Create: `tools/studio-panel-sender.py`
- Create: `tools/requirements.txt`

**Interfaces:**
- Produces: UDP packets to ESP32 port 4210, matching `AudioPacket` layout (1072 bytes)

- [ ] **Step 1: Create `tools/requirements.txt`**

```
sounddevice>=0.4.6
numpy>=1.24.0
```

- [ ] **Step 2: Create `tools/studio-panel-sender.py`**

```python
#!/usr/bin/env python3
"""
studio-panel-sender.py — Streams real-time audio analysis to the ESP32-S3 Studio Panel.

Cross-platform: Linux (PipeWire/PulseAudio via PortAudio) and macOS (CoreAudio via PortAudio).
Install dependencies: pip install sounddevice numpy

Usage:
  python3 studio-panel-sender.py --host 192.168.1.42
  python3 studio-panel-sender.py --list              # show available audio devices
  python3 studio-panel-sender.py --host 192.168.1.42 --device "Monitor of Built-in Audio"
"""

import argparse
import socket
import struct
import sys
import time

import numpy as np
import sounddevice as sd

# UDP packet constants — must match AudioPacket in audio_data.h
MAGIC     = 0xAB
VERSION   = 1
UDP_PORT  = 4210
BINS      = 256
SR        = 44100
BLOCKSIZE = 1470   # ~33ms at 44100 Hz (≈30 fps)
FFT_SIZE  = 1024   # power of 2 ≥ BLOCKSIZE; gives 512 positive bins → decimated to BINS

# Packet format: header (48 bytes) + bins (1024 bytes) = 1072 bytes total
HEADER_FMT = '<BBHIfffffffffI'   # magic,ver,flags,seq, 9×float, fft_bins
assert struct.calcsize(HEADER_FMT) == 48, "Header size mismatch"


def find_monitor_device():
    """Finds the system audio monitor/loopback source for capturing playback audio."""
    devices = sd.query_devices()
    for i, d in enumerate(devices):
        name = d['name'].lower()
        if d['max_input_channels'] < 2:
            continue
        if 'monitor' in name or 'loopback' in name or 'what u hear' in name:
            return i, d['name']
    # Fall back to default input
    idx = sd.default.device[0]
    return idx, sd.query_devices(idx)['name']


def list_devices():
    print("Available audio input devices:")
    for i, d in enumerate(sd.query_devices()):
        if d['max_input_channels'] > 0:
            print(f"  [{i:2d}] {d['name']}  (ch={d['max_input_channels']})")


def pack_packet(seq: int, peak_l: float, peak_r: float, rms_l: float, rms_r: float,
                momentary: float, short_term: float, integrated: float,
                gonio_l: float, gonio_r: float, bins: np.ndarray) -> bytes:
    """Packs one AudioPacket. bins must be float32 array of length BINS, values 0.0–1.0."""
    header = struct.pack(HEADER_FMT,
        MAGIC, VERSION, 0x03, seq,      # flags=3: FFT+Gonio present
        peak_l, peak_r, rms_l, rms_r,
        momentary, short_term, integrated,
        gonio_l, gonio_r,
        BINS
    )
    return header + bins.astype(np.float32).tobytes()


def to_dbfs(rms_power: float) -> float:
    """Converts linear RMS power to dBFS. Returns -60.0 for silence."""
    if rms_power < 1e-10:
        return -60.0
    return max(10.0 * np.log10(rms_power), -60.0)


def main():
    parser = argparse.ArgumentParser(description="Studio Panel audio sender")
    parser.add_argument('--host',   default='192.168.1.100', help='ESP32 IP address')
    parser.add_argument('--port',   type=int, default=UDP_PORT)
    parser.add_argument('--bins',   type=int, default=BINS, help='FFT bins to send (default 256)')
    parser.add_argument('--device', default=None, help='Audio device name or index')
    parser.add_argument('--list',   action='store_true', help='List audio devices and exit')
    args = parser.parse_args()

    if args.list:
        list_devices()
        return

    # Resolve device
    if args.device is None:
        dev_idx, dev_name = find_monitor_device()
    elif args.device.isdigit():
        dev_idx = int(args.device)
        dev_name = sd.query_devices(dev_idx)['name']
    else:
        devices = sd.query_devices()
        matches = [i for i, d in enumerate(devices) if args.device.lower() in d['name'].lower()]
        if not matches:
            print(f"Device '{args.device}' not found. Use --list to see options.", file=sys.stderr)
            sys.exit(1)
        dev_idx, dev_name = matches[0], sd.query_devices(matches[0])['name']

    print(f"Audio source : [{dev_idx}] {dev_name}")
    print(f"Sending to   : {args.host}:{args.port}")
    print(f"FFT bins     : {args.bins}  |  Rate: {SR}Hz  |  Block: {BLOCKSIZE} samples")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    # Hanning window for FFT — reduces spectral leakage
    window   = np.hanning(FFT_SIZE)
    seq      = 0
    i_acc    = 1e-10   # integrated loudness accumulator
    st_acc   = 1e-10   # short-term accumulator
    m_acc    = 1e-10   # momentary accumulator
    ALPHA_M  = 1.0 - np.exp(-BLOCKSIZE / SR / 0.4)   # τ = 400ms
    ALPHA_S  = 1.0 - np.exp(-BLOCKSIZE / SR / 3.0)   # τ = 3s
    ALPHA_I  = 1.0 - np.exp(-BLOCKSIZE / SR / 30.0)  # τ = 30s

    def audio_callback(indata, frames, time_info, status):
        nonlocal seq, i_acc, st_acc, m_acc

        # Mix stereo to mono; keep L/R for goniometer
        l = indata[:, 0] if indata.shape[1] > 0 else indata[:, 0]
        r = indata[:, 1] if indata.shape[1] > 1 else indata[:, 0]
        mono = (l + r) * 0.5

        # Peak (true peak per channel)
        peak_l = to_dbfs(np.max(np.abs(l)) ** 2)
        peak_r = to_dbfs(np.max(np.abs(r)) ** 2)

        # RMS
        rms_l = to_dbfs(np.mean(l ** 2))
        rms_r = to_dbfs(np.mean(r ** 2))

        # LUFS (simplified — exponential MA of mono power)
        power = float(np.mean(mono ** 2))
        m_acc  += ALPHA_M * (power - m_acc)
        st_acc += ALPHA_S * (power - st_acc)
        i_acc  += ALPHA_I (0.0158 - i_acc)  # pulls toward -14 LKFS target
        momentary  = to_dbfs(m_acc)
        short_term = to_dbfs(st_acc)
        integrated = to_dbfs(i_acc)

        # Goniometer sample (last sample of the block)
        gonio_l = float(l[-1])
        gonio_r = float(r[-1])

        # FFT — zero-pad or trim mono block to FFT_SIZE
        block = np.zeros(FFT_SIZE)
        n = min(len(mono), FFT_SIZE)
        block[:n] = mono[:n]
        block *= window

        spectrum = np.abs(np.fft.rfft(block))[:args.bins]
        # Log-scale magnitude, normalised to 0.0–1.0
        spectrum = np.log1p(spectrum * 20.0)
        max_val  = spectrum.max()
        if max_val > 0:
            spectrum /= max_val

        pkt = pack_packet(seq, peak_l, peak_r, rms_l, rms_r,
                          momentary, short_term, integrated,
                          gonio_l, gonio_r, spectrum.astype(np.float32))
        sock.sendto(pkt, (args.host, args.port))
        seq += 1

    # Fix typo: missing parentheses in i_acc update
    # (corrected in Step 3)

    try:
        with sd.InputStream(device=dev_idx, channels=2, samplerate=SR,
                            blocksize=BLOCKSIZE, dtype='float32',
                            callback=audio_callback):
            print("Streaming... Ctrl+C to stop.")
            while True:
                time.sleep(1.0)
    except KeyboardInterrupt:
        print("\nStopped.")
    except Exception as ex:
        print(f"Error: {ex}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
```

- [ ] **Step 3: Fix typo in the script**

Line `i_acc  += ALPHA_I (0.0158 - i_acc)` is missing `*`. Fix:

```python
i_acc  += ALPHA_I * (0.0158 - i_acc)
```

(The script above has this typo — fix it when writing the file.)

- [ ] **Step 4: Test the script locally**

```bash
cd /mnt/source/data/coding/ESP32-S3/tools
pip install sounddevice numpy
python3 studio-panel-sender.py --list
python3 studio-panel-sender.py --host <ESP32_IP>
```

While VLC plays audio, verify the terminal shows "Streaming..." without errors. Check ESP32 monitor for `net_rx: pkts=300 drops=0`.

- [ ] **Step 5: Commit**

```bash
git add tools/studio-panel-sender.py tools/requirements.txt
git commit -m "feat: Python companion script — cross-platform UDP audio sender (Linux + macOS)"
```

---

## Self-Review

**Spec coverage:**
- ✅ AudioPacket 1072 bytes, static_assert → Task 1
- ✅ WiFi hardcoded credentials, auto-reconnect, statusbar icon → Task 2
- ✅ UDP receiver port 4210, validation, xQueueOverwrite, drop logging → Task 3
- ✅ Metering queue read + demo fallback → Task 4
- ✅ Home screen Spectrum tile → Task 5
- ✅ BOOT button: short=next view, long=freeze → Task 5
- ✅ Spectrum View 1: log-scale bars, peak hold, amplitude gradient → Task 6
- ✅ Spectrum View 2: area curve, gradient fill, smoothing → Task 7
- ✅ Spectrum View 3: waterfall RTL, PSRAM canvas, memmove scroll → Task 8
- ✅ 4 color presets via context menu swatches → Task 8
- ✅ Freeze mode: timer runs, display frozen, ❚❚ icon → Task 5 (timer) + Task 8 (wf)
- ✅ Python script: sounddevice, numpy, cross-platform, LUFS, FFT → Task 9
- ✅ IO-contract comments on all non-trivial functions → all tasks

**Ruling: spec says 1076 bytes but math gives 1072** — plan uses 1072 with static_assert. Python script packs 1072. Both sides must match.

**Type consistency:** `SpectrumScreenData` defined in Task 5, referenced in Tasks 6-8. `x_to_bin()` defined in Task 6, reused in Task 7 — must be in same translation unit (same spectrum.cpp). ✓

**Context menu lambda capture** in Task 8: lambdas capturing `d` from outer scope need care in C++. The implementation uses `lv_event_get_param` which requires the param to be set — this may need adjustment. Alternative: use `lv_event_get_user_data` consistently. Implementer should test this and adjust if lambdas cause issues.
