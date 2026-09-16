// =========================================================================
// TFT_eSPI Configuration for ESP32-CAM + ST7789 240x240 7-Pin IPS Display
// =========================================================================
// Copy this file to: Arduino/libraries/TFT_eSPI/User_Setup.h
// OR overwrite your existing User_Setup.h with this configuration.

#define USER_SETUP_INFO "User_Setup_ESP32CAM_ST7789"

// Display Driver
#define ST7789_DRIVER      // ST7789 240x240 IPS Controller

// Display Resolution
#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// Hardware Pinout for ESP32-CAM
#define TFT_MOSI 15       // SDA on display
#define TFT_SCLK 14       // SCL on display
#define TFT_CS   -1       // 7-Pin display has CS tied to GND
#define TFT_DC   12       // DC pin
#define TFT_RST   2       // RES/RESET pin
#define TFT_BL   -1       // Backlight tied directly to 3.3V

// Fonts
#define LOAD_GLCD   // Font 1. Original Adafruit 8 pixel font
#define LOAD_FONT2  // Font 2. Small 16 pixel high font
#define LOAD_FONT4  // Font 4. Medium 26 pixel high font
#define LOAD_FONT6  // Font 6. Large 48 pixel font
#define LOAD_FONT7  // Font 7. 7 segment 48 pixel font
#define LOAD_FONT8  // Font 8. Large 75 pixel font
#define LOAD_GFXFF  // FreeFonts

#define SMOOTH_FONT

// SPI Speed (40MHz provides high frame rates for live viewfinder)
#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000

#define USER_SETUP_LOADED 1

