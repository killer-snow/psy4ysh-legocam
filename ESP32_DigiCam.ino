/*
 * =========================================================================
 * ESP32-CAM DIY DigiCam
 * =========================================================================
 * Hardware:
 *   - AI-Thinker ESP32-CAM (OV2640 + PSRAM)
 *   - ST7789 1.3" 240x240 7-Pin IPS Display
 *       3V3 + BLK -> 3V3
 *       GND       -> GND
 *       SCL       -> GPIO 14
 *       SDA       -> GPIO 15
 *       RESET     -> GPIO 2
 *       DC        -> GPIO 12
 *   - Physical Shutter Button -> GPIO 13 (to GND with INPUT_PULLUP)
 *
 * Web Functionality:
 *   - Standalone Wi-Fi Access Point ("DigiCam-AP" / "123456789")
 *   - Webpage at http://192.168.4.1/
 *   - Pressing the physical shutter button snaps a high-res photo,
 *     which automatically appears on the webpage with a DOWNLOAD button.
 * =========================================================================
 */

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>

#include "camera_pins.h"
#include "web_page.h"

// Wi-Fi Access Point Credentials
const char* AP_SSID = "DigiCam-AP";
const char* AP_PASS = "123456789";

// Display and Web Server instances
TFT_eSPI tft = TFT_eSPI();
httpd_handle_t camera_httpd = NULL;

// Photo Storage in PSRAM
uint8_t* captured_photo_buf = NULL;
size_t captured_photo_len = 0;
volatile uint32_t photo_count = 0;
volatile bool is_capturing = false;

// JPEG decoder callback for ST7789
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

// Viewfinder Overlay: Rules of Thirds Grid + Blinking REC Indicator
void drawModernViewfinder() {
  uint16_t uiColor = TFT_GREEN;
  tft.setTextColor(uiColor, TFT_BLACK);
  tft.setTextSize(1);

  // Garis Bantu Rules of Thirds / Rules of Thirds Grid
  tft.drawLine(tft.width() / 3, 0, tft.width() / 3, tft.height(), TFT_DARKGREEN);
  tft.drawLine((tft.width() / 3) * 2, 0, (tft.width() / 3) * 2, tft.height(), TFT_DARKGREEN);
  tft.drawLine(0, tft.height() / 3, tft.width(), tft.height() / 3, TFT_DARKGREEN);
  tft.drawLine(0, (tft.height() / 3) * 2, tft.width(), (tft.height() / 3) * 2, TFT_DARKGREEN);

  // Indikator REC Berkedip / Blinking REC Indicator
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
  s->set_ae_level(s, 1);         // Balanced exposure compensation (avoids blowing out highlights)

  // Low Light Auto Gain (Capped to reduce grainy multicolored chromatic noise)
  s->set_gain_ctrl(s, 1);        // Enable auto gain
  s->set_gainceiling(s, (gainceiling_t)3); // 3 = 8X / 2 = 4X balanced

  // Optical Artifact Corrections
  s->set_lenc(s, 1);             // Lens vignetting correction
  s->set_bpc(s, 1);              // Black pixel correction
  s->set_wpc(s, 1);              // White pixel correction
  s->set_raw_gma(s, 1);          // Gamma curve correction
}


// Capture Sequence: Switches to UXGA, grabs photo into PSRAM, resets to QVGA
void captureHighResPhoto() {
  is_capturing = true;
  Serial.println("[SHUTTER] Switching sensor to UXGA (1600x1200)...");

  sensor_t *s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_UXGA);
  s->set_quality(s, 10); // Optimized for ultra-fast Wi-Fi transfer with razor-sharp UXGA detail
  delay(120);

  // Discard 2 frames so sensor auto-exposure adapts to UXGA
  for (int i = 0; i < 2; i++) {
    camera_fb_t *junk = esp_camera_fb_get();
    if (junk) esp_camera_fb_return(junk);
    delay(40);
  }

  // Capture High-Res Picture
  camera_fb_t *pic = esp_camera_fb_get();
  if (pic) {
    if (captured_photo_buf != NULL) {
      free(captured_photo_buf);
      captured_photo_buf = NULL;
    }

    captured_photo_len = pic->len;
    captured_photo_buf = (uint8_t*)ps_malloc(captured_photo_len);
    if (!captured_photo_buf) {
      captured_photo_buf = (uint8_t*)malloc(captured_photo_len);
    }

    if (captured_photo_buf) {
      memcpy(captured_photo_buf, pic->buf, captured_photo_len);
      photo_count++; // Signal web page that a new photo is ready
      Serial.printf("[SHUTTER] SUCCESS! UXGA photo captured: %u bytes (%u KB)\n",
                    (unsigned int)captured_photo_len, (unsigned int)(captured_photo_len / 1024));

      // Display the captured photo on the ST7789 screen for 3 seconds
      Serial.println("[VIEWFINDER] Showing captured photo on screen for 3 seconds...");
      TJpgDec.setJpgScale(4); // 1600x1200 / 4 = 400x300 (centered onto 240x240 screen)
      TJpgDec.drawJpg(-80, -30, captured_photo_buf, captured_photo_len);
      TJpgDec.setJpgScale(1); // Reset scale back to 1 for live viewfinder

      // Hold image for 3 seconds while keeping FreeRTOS responsive
      for (int t = 0; t < 30; t++) {
        delay(100);
        yield();
      }
    } else {
      Serial.println("[SHUTTER] ERROR: Failed to allocate PSRAM memory for photo!");
    }

    esp_camera_fb_return(pic);
  } else {
    Serial.println("[SHUTTER] ERROR: esp_camera_fb_get() returned NULL!");
  }

  // Restore sensor back to QVGA for live viewfinder
  s->set_framesize(s, FRAMESIZE_QVGA);
  s->set_quality(s, 14);
  delay(60);

  // Flush 1 frame so sensor cleanly transitions back to live feed
  camera_fb_t *junk2 = esp_camera_fb_get();
  if (junk2) esp_camera_fb_return(junk2);

  is_capturing = false;
}

// =========================================================================
// HTTP SERVER HANDLERS
// =========================================================================

// Serves the Web Photo Gallery Page
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
  return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

// Serves the Latest Captured Photo for Download (Fast 4KB chunked streaming)
static esp_err_t photo_handler(httpd_req_t *req) {
  if (captured_photo_buf == NULL || captured_photo_len == 0) {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=digicam.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

  // Stream in 4096-byte chunks (significantly cuts down TCP packet round trips)
  size_t remaining = captured_photo_len;
  const char *p = (const char *)captured_photo_buf;
  while (remaining > 0) {
    size_t chunk = remaining > 4096 ? 4096 : remaining;
    esp_err_t res = httpd_resp_send_chunk(req, p, chunk);
    if (res != ESP_OK) {
      Serial.printf("[HTTP] Error sending photo chunk: %d\n", res);
      return res;
    }
    p += chunk;
    remaining -= chunk;
  }
  httpd_resp_send_chunk(req, NULL, 0); // End transmission
  return ESP_OK;
}

// Status Polling Endpoint (/status) for Web Auto-Refresh
static esp_err_t status_handler(httpd_req_t *req) {
  char json[128];
  snprintf(json, sizeof(json), "{\"photoId\":%u,\"photoSize\":%u}",
           (unsigned int)photo_count,
           (unsigned int)captured_photo_len);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
  return httpd_resp_send(req, json, strlen(json));
}

// Start HTTP Server
void startWebServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;

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
  }
}

// =========================================================================
// SETUP
// =========================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n==============================");
  Serial.println("   ESP32-CAM DIGICAM START    ");
  Serial.println("==============================");

  pinMode(SHUTTER_PIN, INPUT_PULLUP);
  pinMode(FLASH_PIN, OUTPUT);
  digitalWrite(FLASH_PIN, LOW); // Ensure flash LED is completely OFF

  // Initialize ST7789 Display
  Serial.println("[1/4] Initializing ST7789 Screen...");
  tft.init();
  tft.setRotation(2);      // 240x240 orientation
  tft.invertDisplay(true); // Required for ST7789 IPS accurate colors
  tft.fillScreen(TFT_BLACK);

  // Initialize JPEG Decoder
  TJpgDec.setCallback(tft_output);
  TJpgDec.setSwapBytes(true); // Required for true color depth
  Serial.println("       Screen initialized successfully.");

  // Initialize Camera
  Serial.println("[2/4] Initializing Camera Sensor...");
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
  config.frame_size   = FRAMESIZE_UXGA; // Required: Allocates PSRAM buffer large enough for 1600x1200!
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
    // Switch sensor to QVGA (320x240) for smooth, high-speed viewfinder
    s->set_framesize(s, FRAMESIZE_QVGA);
    s->set_quality(s, 14);
  } else {
    Serial.println("       WARNING: Sensor pointer is NULL!");
  }

  tuneSensorQuality();
  Serial.println("       Camera tuned successfully.");

  // Setup Wi-Fi SoftAP
  Serial.println("[3/4] Starting Wi-Fi Access Point...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm); // Maximum RF transmit power (19.5 dBm) for outdoor range
  Serial.printf("       SSID: %s\n", AP_SSID);
  Serial.printf("       IP:   http://%s/\n", WiFi.softAPIP().toString().c_str());

  // Start HTTP Server
  Serial.println("[4/4] Starting Web Server...");
  startWebServer();
  Serial.println("       Web Server online.");

  Serial.println("==============================");
  Serial.println("   DIGICAM READY TO SHOOT!    ");
  Serial.println("==============================");
}

// =========================================================================
// MAIN LOOP
// =========================================================================
void loop() {
  // 1. Check for Shutter Trigger on GPIO 13
  if (digitalRead(SHUTTER_PIN) == LOW) {
    delay(40); // Debounce
    if (digitalRead(SHUTTER_PIN) == LOW) {
      Serial.println("[SHUTTER] Button pressed! Capturing UXGA photo...");
      captureHighResPhoto();
      Serial.printf("[SHUTTER] Photo saved! Size: %u KB. Ready on webpage.\n", (unsigned int)(captured_photo_len / 1024));
      while (digitalRead(SHUTTER_PIN) == LOW) {
        delay(10);
      }
      return;
    }
  }

  // 2. Viewfinder Display: Render live frame to ST7789 screen
  if (!is_capturing) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      static bool firstSuccess = true;
      if (firstSuccess) {
        Serial.printf("[VIEWFINDER] Camera frames flowing! (Frame size: %u bytes)\n", (unsigned int)fb->len);
        firstSuccess = false;
      }

      // 320x240 QVGA image centered on 240x240 ST7789 screen
      TJpgDec.drawJpg(-40, 0, fb->buf, fb->len);
      esp_camera_fb_return(fb);

      drawModernViewfinder();
    } else {
      static unsigned long lastFailPrint = 0;
      if (millis() - lastFailPrint > 2500) {
        Serial.println("[VIEWFINDER] Warning: Camera returned NULL frame buffer!");
        lastFailPrint = millis();
      }
      delay(10);
    }
  }

  yield();
}
