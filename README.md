# 📷 Psy4ysh LegoCam — ESP32-CAM DIY DigiCam

[![ESP32](https://img.shields.io/badge/Platform-ESP32-blue.svg)](https://www.espressif.com/)
[![Arduino](https://img.shields.io/badge/Framework-Arduino-00979C.svg)](https://www.arduino.cc/)
[![Display](https://img.shields.io/badge/Display-ST7789%20240x240%20IPS-orange.svg)](https://github.com/Bodmer/TFT_eSPI)
[![Sensor](https://img.shields.io/badge/Sensor-OV2640%20UXGA-green.svg)](https://github.com/espressif/esp32-camera)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A compact, standalone, retro-style digital camera built with an **AI-Thinker ESP32-CAM**, a **1.3" 240x240 IPS ST7789 display**, and a tactile shutter push-button. 

Designed for custom enclosures (like a 3D-printed or Lego camera chassis), it features a real-time viewfinder on the physical screen and a companion **Wi-Fi Access Point** that automatically beams full-resolution **UXGA (1600x1200)** photos directly to your smartphone or laptop browser — with zero cloud, zero apps, and zero internet connection required.

---

## ✨ Features

- **Live IPS Viewfinder**: Real-time camera feed rendered onto a 1.3" 240x240 ST7789 IPS display with a composition **Rule-of-Thirds grid** and blinking recording indicator.
- **Dual-Resolution Architecture**:
  - Standby Viewfinder: Smooth **QVGA (320x240)** stream centered on the square screen for maximum frame rate.
  - Shutter Trigger: Dynamically switches the OV2640 ISP to **UXGA (1600x1200)** high quality (`jpeg_quality = 10`) for maximum resolution.
- **Instant Photo Review**: Displays the captured photo on the physical LCD screen for 3 seconds before returning to the live viewfinder.
- **Wi-Fi Companion Studio (SoftAP)**:
  - Creates a local Wi-Fi hotspot (`DigiCam-AP`).
  - Webpage hosted directly at `http://192.168.4.1/`.
  - **Auto-Sync & Download**: Snapping a photo with the physical button automatically transmits the photo to connected phones/PCs and triggers a direct download to your browser's Downloads folder.
- **Hardware-Tuned ISP Pipeline**:
  - Hardware noise reduction filter (`denoise = 1`).
  - Edge sharpening (`sharpness = 2`).
  - Capped analog gain (`gainceiling = 3`) to eliminate chromatic salt-and-pepper noise in low light.
  - Lens shading correction (`lenc`), black pixel correction (`bpc`), and gamma curve correction (`raw_gma`).
- **Power & Heat Optimized**:
  - No continuous video streaming over Wi-Fi, preserving battery life and keeping the ESP32-CAM cool.
  - Maximum RF transmit power configured (`19.5 dBm`) for reliable range.

---

## 📐 Circuit & Wiring Pinout

### 1. ST7789 7-Pin SPI IPS Display (240x240)
| ST7789 Pin | ESP32-CAM Pin | Description |
| :--- | :--- | :--- |
| **VCC** | **3V3** | 3.3V Power |
| **GND** | **GND** | System Ground |
| **SCL / CLK** | **GPIO 14** | SPI Clock (HSPI SCLK) |
| **SDA / MOSI**| **GPIO 15** | SPI Data (HSPI MOSI) |
| **RES / RST** | **GPIO 2** | Hardware Reset |
| **DC** | **GPIO 12** | Data / Command Selection |
| **BLK / LED** | **3V3** | Backlight (tie to 3V3 for full brightness) |
| **CS** | *N/A* | 7-pin modules have CS internally tied to GND |

### 2. Shutter Button & Flash
| Component | ESP32-CAM Pin | Wiring Details |
| :--- | :--- | :--- |
| **Shutter Button** | **GPIO 13** | Momentary push-button connected between **GPIO 13** and **GND** (uses internal `INPUT_PULLUP`). |
| **Onboard Flash** | **GPIO 4** | Controlled via software; kept LOW to prevent unwanted glowing. |

> [!NOTE]
> All used GPIOs (`12, 13, 14, 15, 2`) are broken out on the ESP32-CAM header and do not collide with the OV2640 camera bus or the 8MB external PSRAM (`GPIO 16/17`).

---

## 🛠️ Software Setup & Installation

### 1. Required Arduino Libraries
Install the following libraries via the Arduino IDE Library Manager (**Sketch > Include Library > Manage Libraries...**):
1. **`TFT_eSPI`** by Bodmer
2. **`TJpg_Decoder`** by Bodmer

### 2. Configuring `TFT_eSPI`
Because `TFT_eSPI` supports dozens of display chips and pinouts, you must point it to the included custom setup file:

1. Copy the included [`User_Setup_digicam.h`](User_Setup_digicam.h) into your Arduino library folder:
   ```text
   Documents/Arduino/libraries/TFT_eSPI/User_Setup_digicam.h
   ```
2. Open `Documents/Arduino/libraries/TFT_eSPI/User_Setup_Select.h`:
   - Uncomment line 29:
     ```cpp
     #include <User_Setup_digicam.h>
     ```
   - Make sure all other setup files (including `Setup24_ST7789.h` or default `User_Setup.h`) are **commented out**.

---

## 💻 Arduino IDE Board Settings

In the Arduino IDE menu under **Tools**, apply these exact settings:

| Setting | Recommended Value | Why? |
| :--- | :--- | :--- |
| **Board** | `AI Thinker ESP32-CAM` | Matches camera pinout & clock configurations |
| **CPU Frequency** | `240MHz (WiFi/BT)` | Maximum processing speed for JPEG rendering |
| **Flash Frequency**| `80MHz` | Fast SPI flash access |
| **Flash Mode** | `QIO` | Quad I/O speed |
| **Partition Scheme** | `Huge APP (3MB No OTA/1MB SPIFFS)` | **Crucial:** Standard partition is too small for WebServer + Camera + Display code |
| **PSRAM** | **`Enabled`** | **Crucial:** Required to allocate high-resolution 1600x1200 frame buffers |
| **Upload Speed** | `115200` or `921600` | Standard serial flashing baud rate |

---

## ⚡ Flashing the ESP32-CAM

1. Connect an FTDI USB-to-TTL programmer to the ESP32-CAM:
   - FTDI `VCC (5V)` -> ESP32-CAM `5V`
   - FTDI `GND` -> ESP32-CAM `GND`
   - FTDI `TX` -> ESP32-CAM `U0R (GPIO 3)`
   - FTDI `RX` -> ESP32-CAM `U0T (GPIO 1)`
2. **Bootloader Mode**: Connect a jumper wire between **GPIO 0 and GND**, then press the **RST** button.
3. Click **Upload** in the Arduino IDE.
4. **Run Mode**: Once the upload reaches 100%, **disconnect GPIO 0 from GND**, and press the **RST** button again to start the camera.

---

## 🚀 How to Use

1. **Power On**:
   - Power the camera via the 5V pin using a battery pack or step-up booster.
   - The ST7789 screen turns on immediately and presents the live viewfinder with rule-of-thirds composition aids.
2. **Shooting a Photo**:
   - Aim your subject and press the shutter button on **GPIO 13**.
   - The camera switches to UXGA, takes a full 1600x1200 shot, and displays a 3-second preview of the photo on the LCD before returning to the live viewfinder.
3. **Downloading Photos Wirelessly**:
   - On your smartphone, tablet, or laptop, connect to the Wi-Fi network:
     - **SSID**: `DigiCam-AP`
     - **Password**: `123456789`
   - Open your browser and navigate to:
     ```text
     http://192.168.4.1/
     ```
   - When you press the physical shutter button, the photo automatically streams to the page and downloads to your device! A manual **DOWNLOAD PHOTO** button is also available.

---

## 🔍 Getting Maximum Image Quality (Hardware Focus Trick)

If your photos look blurry or soft out of the box, perform these two physical adjustments:

1. **Peel Off the Lens Film**: Check the tiny glass lens. Brand new OV2640 modules often ship with a small, nearly invisible protective film that causes a milky, hazy look. Peel it off.
2. **Adjust the Manual Focus Ring**:
   - Most factory OV2640 lenses are glued or adjusted for macro distance (10–15 cm).
   - Point your camera at an object across the room (1–3 meters away).
   - Gently twist the outer brass ring of the lens counter-clockwise by half a turn to one full turn while watching the ST7789 screen.
   - You will see the image snap from blurry to razor-sharp!

---

## 📂 Project File Structure

```text
ESP32_DigiCam/
├── ESP32_DigiCam.ino       # Core firmware: camera driver, ST7789 engine, HTTP server & state machine
├── camera_pins.h           # Hardware pin definitions for AI-Thinker ESP32-CAM
├── web_page.h              # Embedded companion web application (PROGMEM HTML/CSS/JS)
├── User_Setup_digicam.h    # Preconfigured TFT_eSPI driver file for 7-pin ST7789
└── README.md               # Complete project documentation & hardware guide
```

---

## 🤝 Credits & Acknowledgments

- [Espressif](https://github.com/espressif/esp32-camera) for the official `esp32-camera` driver.
- [Bodmer](https://github.com/Bodmer) for the high-performance [`TFT_eSPI`](https://github.com/Bodmer/TFT_eSPI) and [`TJpg_Decoder`](https://github.com/Bodmer/TJpg_Decoder) libraries.
- Inspired by the open-source ESP32 camera maker community.

---

## 📄 License
This project is open-source and available under the [MIT License](LICENSE). Feel free to modify, build your own Lego enclosures, and share!
