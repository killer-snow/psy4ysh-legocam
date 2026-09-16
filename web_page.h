#ifndef WEB_PAGE_H
#define WEB_PAGE_H

#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DigiCam Photos</title>
<style>
  * { box-sizing: border-box; }
  body {
    text-align: center;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    background: #111;
    color: #eee;
    margin: 0;
    padding: 16px;
  }
  h2 { margin: 8px 0 4px; font-weight: 700; letter-spacing: 0.5px; }
  .photo-card {
    position: relative;
    display: inline-block;
    max-width: 95%;
    width: 480px;
    background: #1a1a1a;
    border: 2px solid #333;
    border-radius: 12px;
    overflow: hidden;
    margin-top: 12px;
    box-shadow: 0 8px 24px rgba(0,0,0,0.6);
  }
  #photo {
    width: 100%;
    display: none;
    border-radius: 10px 10px 0 0;
    background: #000;
  }
  #placeholder {
    padding: 50px 20px;
    color: #888;
    font-size: 15px;
    line-height: 1.5;
  }
  #placeholder .icon {
    font-size: 48px;
    margin-bottom: 12px;
    display: block;
  }
  .action-area {
    padding: 16px;
    display: none;
    background: #181818;
    border-top: 1px solid #282828;
  }
  .badge-saved {
    display: inline-block;
    padding: 6px 14px;
    font-size: 13px;
    font-weight: 600;
    border-radius: 20px;
    background: #064e3b;
    color: #34d399;
    border: 1px solid #059669;
    margin-bottom: 10px;
  }
  a.download-btn {
    display: inline-block;
    padding: 12px 24px;
    font-size: 15px;
    font-weight: 700;
    border-radius: 8px;
    background: #2563eb;
    color: white;
    text-decoration: none;
    box-shadow: 0 4px 12px rgba(37,99,235,0.4);
    transition: background 0.2s;
  }
  a.download-btn:active { background: #1d4ed8; }
  #info {
    color: #9ca3af;
    font-size: 13px;
    margin-top: 8px;
  }
  .pulse-dot {
    display: inline-block;
    width: 8px;
    height: 8px;
    background: #10b981;
    border-radius: 50%;
    margin-right: 6px;
    vertical-align: middle;
    animation: pulse 1.5s infinite;
  }
  @keyframes pulse {
    0% { opacity: 0.3; }
    50% { opacity: 1; }
    100% { opacity: 0.3; }
  }
</style>
</head>
<body>
  <h2>DigiCam</h2>
  <div id="info"><span class="pulse-dot"></span>Connected • Photos auto-save to device</div>

  <div class="photo-card">
    <div id="placeholder">
      <span class="icon">📷</span>
      <span id="promptText">Press the shutter button on your DigiCam to take a photo.</span>
    </div>
    <img id="photo" alt="Captured Photo">
    <div id="actionArea" class="action-area">
      <div class="badge-saved">✅ Auto-saved to Downloads</div>
      <div>
        <a id="downloadBtn" class="download-btn" href="/photo.jpg" download="digicam.jpg">💾 DOWNLOAD PHOTO</a>
      </div>
      <div id="meta" style="color:#aaa; font-size:13px; margin-top:10px;"></div>
    </div>
  </div>

<script>
  const photo = document.getElementById('photo');
  const placeholder = document.getElementById('placeholder');
  const promptText = document.getElementById('promptText');
  const actionArea = document.getElementById('actionArea');
  const downloadBtn = document.getElementById('downloadBtn');
  const meta = document.getElementById('meta');
  let currentPhotoId = 0;
  let isLoading = false;

  function checkNewPhoto() {
    if (isLoading) return;

    fetch('/status?t=' + Date.now())
      .then(res => res.json())
      .then(data => {
        if (data.photoId && data.photoId !== currentPhotoId && data.photoSize > 0) {
          currentPhotoId = data.photoId;
          isLoading = true;
          promptText.innerText = "New photo snapped! Downloading " + Math.round(data.photoSize / 1024) + " KB to device...";

          // Fetch the JPEG blob directly
          fetch('/photo.jpg?t=' + Date.now())
            .then(res => {
              if (!res.ok) throw new Error("HTTP " + res.status);
              return res.blob();
            })
            .then(blob => {
              isLoading = false;
              const blobUrl = URL.createObjectURL(blob);
              photo.src = blobUrl;
              placeholder.style.display = 'none';
              photo.style.display = 'block';
              actionArea.style.display = 'block';
              downloadBtn.href = blobUrl;
              downloadBtn.download = 'digicam_' + Date.now() + '.jpg';
              meta.innerText = 'UXGA (1600x1200) • ' + Math.round(blob.size / 1024) + ' KB';

              // Auto-trigger download directly to device
              const a = document.createElement('a');
              a.href = blobUrl;
              a.download = 'digicam_' + Date.now() + '.jpg';
              document.body.appendChild(a);
              a.click();
              setTimeout(() => document.body.removeChild(a), 500);
            })
            .catch(err => {
              isLoading = false;
              promptText.innerText = "Error receiving image. Taking another shot will retry.";
            });
        }
      })
      .catch(() => {});
  }

  // Poll camera every 1000ms for newly snapped photos
  setInterval(checkNewPhoto, 1000);
  checkNewPhoto();
</script>
</body>
</html>
)rawliteral";

#endif // WEB_PAGE_H
