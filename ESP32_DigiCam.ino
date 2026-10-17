/*
 * =========================================================================
 * ESP32-CAM DIY DigiCam — Production Engine & Hardened Network Stack
 * =========================================================================
 * Hardware:
 *   - AI-Thinker ESP32-CAM (OV2640 + 4MB/8MB PSRAM)
 *   - ST7789 1.3" 240x240 7-Pin IPS Display
 *       3V3 + BLK -> 3V3
 *       GND       -> GND
 *       SCL       -> GPIO 14 (HSPI SCLK)
 *       SDA       -> GPIO 15 (HSPI MOSI)
 *       RESET     -> GPIO 2
 *       DC        -> GPIO 12
 *   - Physical Shutter Button -> GPIO 13 (to GND with INPUT_PULLUP)
 *
 * Concurrency & Memory Architecture:
 *   - Pre-allocated PSRAM Triple-Buffer Pool (3 x 384 KB fixed slots)
 *   - Reference-counted reader lock prevents use-after-free and write collisions
 *   - Fully non-blocking review state machine (zero DNS starvation)
 *   - Fast shutter re-triggering with zero lag
 *   - Explicit SoftAP static IP configuration & RFC 9112 compliant streaming
 * =========================================================================
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include "esp_http_server.h"
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "camera_pins.h"
#include "web_page.h"

// Wi-Fi Access Point Credentials
const char* AP_SSID = "DigiCam-AP";
const char* AP_PASS = "123456789";

// Captive Portal Offline DNS Server (Port 53)
DNSServer dnsServer;
const byte DNS_PORT = 53;

// Display and Web Server instances
TFT_eSPI tft = TFT_eSPI();
httpd_handle_t camera_httpd = NULL;

// =========================================================================
// THREAD-SAFE PSRAM TRIPLE-BUFFER POOL
// =========================================================================
#define NUM_PHOTO_SLOTS 3
#define MAX_SLOT_SIZE (384 * 1024) // 384 KB per slot in PSRAM (ample for UXGA Q=10)

typedef struct {
  uint8_t* buf;
  size_t len;
  uint32_t id;
  volatile int readers; // Active HTTP streaming clients
} PhotoSlot;

static PhotoSlot photo_slots[NUM_PHOTO_SLOTS];
static volatile int active_slot_idx = -1;
static volatile uint32_t global_photo_id = 0;
static SemaphoreHandle_t photo_mutex = NULL;

// Camera State Machine
enum CameraState {
  STATE_VIEWFINDER,
  STATE_REVIEW_DISPLAY
};
static volatile CameraState camera_state = STATE_VIEWFINDER;
static unsigned long review_start_time = 0;

// JPEG decoder callback with screen boundary guards
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return false; // Below screen: no later block can be visible
  if (x >= tft.width() || x + w <= 0 || y + h <= 0) return true; // Skip off-screen block, continue decoding
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

// Viewfinder Overlay: Rule-of-Thirds Grid + Blinking REC Indicator
void drawModernViewfinder() {
  uint16_t uiColor = TFT_GREEN;
  tft.setTextColor(uiColor, TFT_BLACK);
  tft.setTextSize(1);

  // Rule-of-Thirds Grid
  tft.drawLine(tft.width() / 3, 0, tft.width() / 3, tft.height(), TFT_DARKGREEN);
  tft.drawLine((tft.width() / 3) * 2, 0, (tft.width() / 3) * 2, tft.height(), TFT_DARKGREEN);
  tft.drawLine(0, tft.height() / 3, tft.width(), tft.height() / 3, TFT_DARKGREEN);
  tft.drawLine(0, (tft.height() / 3) * 2, tft.width(), (tft.height() / 3) * 2, TFT_DARKGREEN);

  // Blinking REC Indicator
  int offset = 12;
  if (millis() % 1000 < 500) {
    tft.fillCircle(tft.width() - offset - 40, tft.height() - offset - 3, 3, TFT_RED);
  }
}

// Sensor Image Quality Optimizer (OV2640)
void tuneSensorQuality() {
  sensor_t *s = esp_camera_sensor_get();
  if (s == NULL) return;

  s->set_vflip(s, 1);

  s->set_brightness(s, 1);       // -2 to 2 (lifts shadow areas)
  s->set_contrast(s, 1);         // -2 to 2 (enhances definition)
  s->set_saturation(s, 1);       // -2 to 2 (rich colors)
  s->set_sharpness(s, 2);        // Max sharpness (2) for crisp fine details
  s->set_denoise(s, 1);          // Enable hardware noise reduction filter

  // Auto White Balance
  s->set_whitebal(s, 1);         // Enable AWB
  s->set_awb_gain(s, 1);         // Enable AWB gain
  s->set_wb_mode(s, 0);          // 0 = Auto

  // Auto Exposure
  s->set_exposure_ctrl(s, 1);    // Enable auto exposure
  s->set_aec2(s, 1);             // Enhanced DSP AEC algorithm
  s->set_ae_level(s, 1);         // Balanced exposure compensation

  // Low Light Auto Gain (Capped to reduce grainy multicolored chromatic noise)
  s->set_gain_ctrl(s, 1);        // Enable auto gain
  s->set_gainceiling(s, (gainceiling_t)3); // 3 = 8X / 2 = 4X balanced

  // Optical Artifact Corrections
  s->set_lenc(s, 1);             // Lens vignetting correction
  s->set_bpc(s, 1);              // Black pixel correction
  s->set_wpc(s, 1);              // White pixel correction
  s->set_raw_gma(s, 1);          // Gamma curve correction
}

// Select an available standby buffer slot from the PSRAM pool safely
int getAvailableSlot() {
  if (photo_mutex == NULL) return -1;
  xSemaphoreTake(photo_mutex, portMAX_DELAY);

  int chosen = -1;
  // Pick a standby slot that is NOT currently active and has NO active readers
  for (int i = 0; i < NUM_PHOTO_SLOTS; i++) {
    if (i != active_slot_idx && photo_slots[i].readers == 0) {
      chosen = i;
      break;
    }
  }

  xSemaphoreGive(photo_mutex);
  return chosen;
}

// Capture Sequence: Switches to UXGA, grabs photo into pre-allocated PSRAM, displays review
void captureHighResPhoto() {
  int target_slot = getAvailableSlot();
  if (target_slot == -1) {
    Serial.println("[SHUTTER] All slots busy streaming to network; capture skipped.");
    return;
  }

  Serial.printf("[SHUTTER] Capturing into PSRAM slot %d...\n", target_slot);

  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;

  s->set_framesize(s, FRAMESIZE_UXGA);
  s->set_quality(s, 10);
  delay(100);

  // Grab photo with resolution validation to reject stale QVGA frames
  camera_fb_t *pic = NULL;
  for (int attempt = 0; attempt < 4; attempt++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      if (fb->width == 1600 && fb->height == 1200) {
        pic = fb;
        break;
      }
      // Frame was still QVGA or corrupted, discard and retry
      esp_camera_fb_return(fb);
    }
    delay(30);
  }

  // Validate frame size: reject oversized frames cleanly instead of truncating JPEG
  if (pic) {
    if (pic->len > MAX_SLOT_SIZE) {
      Serial.printf("[SHUTTER] ERROR: Frame %u bytes exceeds MAX_SLOT_SIZE (%u bytes)!\n",
                    (unsigned int)pic->len, (unsigned int)MAX_SLOT_SIZE);
      esp_camera_fb_return(pic);
      pic = NULL;
    }
  }

  if (pic) {
    size_t copy_len = pic->len;

    // Copy frame directly into pre-allocated PSRAM buffer
    memcpy(photo_slots[target_slot].buf, pic->buf, copy_len);
    photo_slots[target_slot].len = copy_len;

    // Atomically publish new active photo
    xSemaphoreTake(photo_mutex, portMAX_DELAY);
    global_photo_id++;
    photo_slots[target_slot].id = global_photo_id;
    active_slot_idx = target_slot;
    xSemaphoreGive(photo_mutex);

    Serial.printf("[SHUTTER] SUCCESS! UXGA photo #%u captured: %u bytes (%u KB)\n",
                  (unsigned int)global_photo_id,
                  (unsigned int)copy_len,
                  (unsigned int)(copy_len / 1024));

    // Render scaled review onto ST7789 display (400x300 centered on 240x240)
    TJpgDec.setJpgScale(4);
    TJpgDec.drawJpg(-80, -30, photo_slots[target_slot].buf, photo_slots[target_slot].len);
    TJpgDec.setJpgScale(1);

    esp_camera_fb_return(pic);
  } else {
    Serial.println("[SHUTTER] ERROR: Failed to capture valid UXGA frame!");
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.setTextSize(2);
    tft.drawString("CAPTURE FAILED", 35, 110);
  }

  // Restore sensor back to QVGA for live viewfinder
  s->set_framesize(s, FRAMESIZE_QVGA);
  s->set_quality(s, 14);
  delay(50);

  // Flush 1 frame so sensor cleanly transitions back to live feed
  camera_fb_t *junk = esp_camera_fb_get();
  if (junk) esp_camera_fb_return(junk);

  // Transition to review display state (non-blocking)
  camera_state = STATE_REVIEW_DISPLAY;
  review_start_time = millis();
}

// =========================================================================
// HTTP SERVER HANDLERS (THREAD-SAFE & RFC 9112 COMPLIANT)
// =========================================================================

// Serves the Web Photo Gallery Page
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

// Serves the Latest Captured Photo (RFC 9112 compliant full-body send)
static esp_err_t photo_handler(httpd_req_t *req) {
  int slot_idx = -1;
  uint8_t *send_buf = NULL;
  size_t send_len = 0;
  uint32_t served_id = 0;

  // Safely acquire reader lock on the active slot
  xSemaphoreTake(photo_mutex, portMAX_DELAY);
  if (active_slot_idx >= 0 && active_slot_idx < NUM_PHOTO_SLOTS) {
    slot_idx = active_slot_idx;
    send_buf = photo_slots[slot_idx].buf;
    send_len = photo_slots[slot_idx].len;
    served_id = photo_slots[slot_idx].id;
    photo_slots[slot_idx].readers++; // Increment reader count
  }
  xSemaphoreGive(photo_mutex);

  if (slot_idx == -1 || send_buf == NULL || send_len == 0) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  char id_str[16];
  snprintf(id_str, sizeof(id_str), "%u", (unsigned int)served_id);

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=digicam.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Access-Control-Expose-Headers", "X-Photo-Id");
  httpd_resp_set_hdr(req, "X-Photo-Id", id_str);
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

  // httpd_resp_send automatically sets Content-Length and sends entire body cleanly
  esp_err_t res = httpd_resp_send(req, (const char *)send_buf, send_len);
  if (res != ESP_OK) {
    Serial.printf("[HTTP] Error sending photo: %d\n", res);
  }

  // Always release the reader count!
  xSemaphoreTake(photo_mutex, portMAX_DELAY);
  photo_slots[slot_idx].readers--;
  if (photo_slots[slot_idx].readers < 0) {
    photo_slots[slot_idx].readers = 0;
  }
  xSemaphoreGive(photo_mutex);

  return res;
}

// Status Polling Endpoint (/status) for Web Auto-Refresh
static esp_err_t status_handler(httpd_req_t *req) {
  uint32_t current_id = 0;
  size_t current_size = 0;

  xSemaphoreTake(photo_mutex, portMAX_DELAY);
  if (active_slot_idx >= 0 && active_slot_idx < NUM_PHOTO_SLOTS) {
    current_id = photo_slots[active_slot_idx].id;
    current_size = photo_slots[active_slot_idx].len;
  }
  xSemaphoreGive(photo_mutex);

  char json[128];
  snprintf(json, sizeof(json), "{\"photoId\":%u,\"photoSize\":%u}",
           (unsigned int)current_id,
           (unsigned int)current_size);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
  return httpd_resp_send(req, json, strlen(json));
}

// Captive Portal Redirect Handler (Intercepts connectivity checks)
static esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t error) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

// Start HTTP Server with hardened socket stack configuration
void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.lru_purge_enable = true;
  config.max_open_sockets = 5;
  config.stack_size = 8192;       // Double default stack size to prevent task overflow
  config.send_wait_timeout = 10;  // 10s timeout prevents premature socket aborts
  config.recv_wait_timeout = 10;

  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t photo_uri = {
    .uri       = "/photo.jpg",
    .method    = HTTP_GET,
    .handler   = photo_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t status_uri = {
    .uri       = "/status",
    .method    = HTTP_GET,
    .handler   = status_handler,
    .user_ctx  = NULL
  };

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &photo_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_err_handler(camera_httpd, HTTPD_404_NOT_FOUND, http_404_error_handler);
  }
}

// =========================================================================
// SETUP
// =========================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n============================================");
  Serial.println("   ESP32-CAM DIGICAM (PRODUCTION ENGINE)    ");
  Serial.println("============================================");

  pinMode(SHUTTER_PIN, INPUT_PULLUP);
  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW); // Flash LED explicitly OFF

  // 1. Initialize ST7789 Display
  Serial.println("[1/5] Initializing ST7789 Screen...");
  tft.init();
  tft.setRotation(0);
  tft.invertDisplay(true);
  tft.fillScreen(TFT_BLACK);
  tft.setViewport(0, 0, 240, 240); // Enable hardware scissoring/clipping

  TJpgDec.setCallback(tft_output);
  TJpgDec.setSwapBytes(true);
  Serial.println("       Screen initialized successfully.");

  // 2. Initialize Thread Safety Primitives & Pre-allocate PSRAM Pool
  Serial.println("[2/5] Pre-allocating PSRAM Triple-Buffer Pool...");
  photo_mutex = xSemaphoreCreateMutex();
  if (photo_mutex == NULL) {
    Serial.println("       CRITICAL: Failed to create photo mutex!");
    tft.fillScreen(TFT_RED);
    while (true) delay(1000);
  }

  for (int i = 0; i < NUM_PHOTO_SLOTS; i++) {
    photo_slots[i].buf = (uint8_t*)ps_malloc(MAX_SLOT_SIZE);
    if (!photo_slots[i].buf) {
      Serial.printf("       CRITICAL: Failed to allocate PSRAM slot %d (%d KB)!\n", i, MAX_SLOT_SIZE / 1024);
      tft.fillScreen(TFT_RED);
      while (true) delay(1000);
    } else {
      Serial.printf("       Slot %d initialized (%d KB in PSRAM).\n", i, MAX_SLOT_SIZE / 1024);
    }
    photo_slots[i].len = 0;
    photo_slots[i].id = 0;
    photo_slots[i].readers = 0;
  }

  // 3. Initialize Camera Sensor
  Serial.println("[3/5] Initializing Camera Sensor...");
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_UXGA; // Pre-allocates driver buffers for 1600x1200
  config.jpeg_quality = 10;
  config.fb_count     = 2;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("ERROR: Camera init failed: 0x%x\n", err);
    tft.fillScreen(TFT_RED);
    while (true) delay(1000);
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL) {
    Serial.printf("       Camera Sensor Detected! Model PID: 0x%04X\n", s->id.PID);
    s->set_framesize(s, FRAMESIZE_QVGA); // Default to QVGA for live viewfinder
    s->set_quality(s, 14);
  }

  tuneSensorQuality();
  Serial.println("       Camera tuned successfully.");

  // 4. Setup Wi-Fi SoftAP & Static IP Configuration
  Serial.println("[4/5] Starting Wi-Fi Access Point & Offline DNS...");
  WiFi.mode(WIFI_AP);

  // Explicitly configure DHCP static IP parameters (avoids 0.0.0.0 gateway on mobile devices)
  IPAddress apIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, gateway, subnet);

  WiFi.softAP(AP_SSID, AP_PASS, 6);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Serial.printf("       SSID: %s\n", AP_SSID);
  Serial.printf("       IP:   http://%s/\n", WiFi.softAPIP().toString().c_str());

  // Start mDNS responder (allows browsing to http://digicam.local/)
  if (MDNS.begin("digicam")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("       mDNS active: http://digicam.local/");
  }

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println("       Offline DNS Server active.");

  // 5. Start Web Server
  Serial.println("[5/5] Starting Web Server...");
  startWebServer();
  Serial.println("       Web Server online.");

  Serial.println("============================================");
  Serial.println("   DIGICAM READY (RESPONSIVE STATE MACHINE) ");
  Serial.println("============================================");
}

// =========================================================================
// MAIN LOOP (NON-BLOCKING RESPONSIVE STATE MACHINE)
// =========================================================================
void loop() {
  // Pump DNS server requests in burst to keep captive portal instant & alive
  for (int i = 0; i < 3; i++) {
    dnsServer.processNextRequest();
  }

  // 1. Shutter Button Detection (Instant re-trigger supported even during review)
  if (digitalRead(SHUTTER_PIN) == LOW) {
    delay(30); // Debounce
    if (digitalRead(SHUTTER_PIN) == LOW) {
      Serial.println("[SHUTTER] Physical button pressed!");
      captureHighResPhoto();

      // Wait for physical button release while keeping DNS responsive
      while (digitalRead(SHUTTER_PIN) == LOW) {
        dnsServer.processNextRequest();
        delay(10);
      }
      return;
    }
  }

  // 2. State Machine Handling
  if (camera_state == STATE_REVIEW_DISPLAY) {
    // Review timer: return to live viewfinder after 2.5 seconds
    if (millis() - review_start_time >= 2500) {
      camera_state = STATE_VIEWFINDER;
    }
    delay(10);
    yield();
    return;
  }

  // 3. Live Viewfinder State
  if (camera_state == STATE_VIEWFINDER) {
    camera_fb_t *fb = esp_camera_fb_get();
    dnsServer.processNextRequest();

    if (fb) {
      // 320x240 QVGA image centered on 240x240 ST7789 screen
      TJpgDec.drawJpg(-40, 0, fb->buf, fb->len);
      dnsServer.processNextRequest();

      esp_camera_fb_return(fb);
      drawModernViewfinder();
      delay(8); // Breathing room for Wi-Fi and WebServer packet processing
    } else {
      delay(10);
    }
  }

  yield();
}
