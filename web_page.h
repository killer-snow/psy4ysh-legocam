#ifndef WEB_PAGE_H
#define WEB_PAGE_H

#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DigiCam Companion</title>
<style>
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  body {
    text-align: center;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    background: #0d0d0d;
    color: #f3f4f6;
    margin: 0;
    padding: 16px 12px;
  }
  .header {
    display: flex;
    align-items: center;
    justify-content: space-between;
    max-width: 480px;
    margin: 0 auto 12px;
    padding: 0 4px;
  }
  h1 { margin: 0; font-size: 20px; font-weight: 800; letter-spacing: -0.5px; }
  .badge-status {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: 12px;
    font-weight: 600;
    padding: 4px 10px;
    border-radius: 999px;
    background: #1f2937;
    color: #9ca3af;
  }
  .pulse-dot {
    width: 8px;
    height: 8px;
    background: #10b981;
    border-radius: 50%;
    animation: pulse 1.5s infinite;
  }
  @keyframes pulse {
    0%, 100% { opacity: 0.3; transform: scale(0.9); }
    50% { opacity: 1; transform: scale(1.1); }
  }
  .photo-card {
    position: relative;
    max-width: 480px;
    margin: 0 auto;
    background: #18181b;
    border: 1px solid #27272a;
    border-radius: 16px;
    overflow: hidden;
    box-shadow: 0 12px 32px rgba(0,0,0,0.7);
  }
  #photo {
    width: 100%;
    display: none;
    background: #000;
  }
  #placeholder {
    padding: 64px 20px;
    color: #71717a;
    font-size: 14px;
    line-height: 1.6;
  }
  #placeholder .icon {
    font-size: 54px;
    margin-bottom: 12px;
    display: block;
  }
  .action-area {
    padding: 16px;
    display: none;
    background: #18181b;
    border-top: 1px solid #27272a;
  }
  .btn-group {
    display: flex;
    flex-direction: column;
    gap: 10px;
  }
  .btn {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 8px;
    width: 100%;
    padding: 14px;
    font-size: 15px;
    font-weight: 700;
    border-radius: 10px;
    border: none;
    cursor: pointer;
    text-decoration: none;
    transition: all 0.15s ease;
  }
  .btn-download {
    background: #2563eb;
    color: #ffffff;
    box-shadow: 0 4px 14px rgba(37,99,235,0.4);
  }
  .btn-download:active { background: #1d4ed8; }
  .btn-share {
    background: #10b981;
    color: #ffffff;
    box-shadow: 0 4px 14px rgba(16,185,129,0.4);
  }
  .btn-share:active { background: #059669; }
  #statusBanner {
    font-size: 13px;
    color: #a1a1aa;
    margin-bottom: 12px;
  }
  #meta {
    font-size: 12px;
    color: #71717a;
    margin-top: 12px;
  }
  .tip-box {
    max-width: 480px;
    margin: 18px auto 0;
    font-size: 12px;
    color: #71717a;
    line-height: 1.5;
    padding: 0 8px;
  }
  .tip-box a { color: #60a5fa; text-decoration: none; }
</style>
</head>
<body>
  <div class="header">
    <h1>📷 DigiCam</h1>
    <div class="badge-status">
      <span class="pulse-dot"></span>
      <span id="connState">Live</span>
    </div>
  </div>

  <div class="photo-card">
    <div id="placeholder">
      <span class="icon">📸</span>
      <span id="promptText">Aim and click the shutter button on your DigiCam to shoot.</span>
    </div>
    <img id="photo" alt="Captured UXGA Photo">
    <div id="actionArea" class="action-area">
      <div id="statusBanner">✅ Photo Ready!</div>
      <div class="btn-group">
        <a id="downloadBtn" class="btn btn-download" href="/photo.jpg" download="digicam.jpg">💾 Download JPG</a>
        <button id="shareBtn" class="btn btn-share" style="display:none;">📤 Share to Apps</button>
      </div>
      <div id="meta"></div>
    </div>
  </div>

  <div class="tip-box">
    💡 <b>Tip:</b> When prompted <i>"No Internet Connection"</i>, select <b>Keep Wi-Fi</b>.<br>
    If photos don't load, temporarily switch off <b>Cellular Data</b> or visit <a href="http://digicam.local">http://digicam.local</a>.
  </div>

<script>
  const photo = document.getElementById('photo');
  const placeholder = document.getElementById('placeholder');
  const promptText = document.getElementById('promptText');
  const actionArea = document.getElementById('actionArea');
  const shareBtn = document.getElementById('shareBtn');
  const downloadBtn = document.getElementById('downloadBtn');
  const meta = document.getElementById('meta');
  const statusBanner = document.getElementById('statusBanner');
  const connState = document.getElementById('connState');

  let currentPhotoId = 0;
  let isPolling = false;
  let activeObjectUrl = null;
  let currentFile = null;

  downloadBtn.onclick = (e) => {
    if (!activeObjectUrl) {
      e.preventDefault();
      alert("No photo captured yet! Press the physical shutter button on your DigiCam to shoot.");
    }
  };

  async function fetchWithTimeout(url, ms, reader) {
    const ctrl = new AbortController();
    const timer = setTimeout(() => ctrl.abort(), ms);
    try {
      const res = await fetch(url, { cache: 'no-store', signal: ctrl.signal });
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const body = await reader(res);
      return { res, body };
    } finally {
      clearTimeout(timer);
    }
  }

  async function checkStatus() {
    if (isPolling) return;
    isPolling = true;

    try {
      const { body: data } = await fetchWithTimeout('/status?t=' + Date.now(), 4000, r => r.json());
      connState.innerText = "Live";

      if (data.photoId && data.photoId !== currentPhotoId && data.photoSize > 0) {
        const targetId = data.photoId;
        const recvMsg = "Receiving photo (" + Math.round(data.photoSize / 1024) + " KB)...";
        promptText.innerText = recvMsg;
        statusBanner.innerText = "⏳ " + recvMsg;

        try {
          const { res: photoRes, body: blob } = await fetchWithTimeout('/photo.jpg?t=' + Date.now(), 20000, r => r.blob());
          const servedId = parseInt(photoRes.headers.get('X-Photo-Id'), 10) || targetId;

          // Revoke previous blob URL to prevent memory leaks
          if (activeObjectUrl) {
            URL.revokeObjectURL(activeObjectUrl);
          }

          activeObjectUrl = URL.createObjectURL(blob);
          currentPhotoId = servedId; // Commit the ID of the image actually received!
          
          photo.src = activeObjectUrl;
          placeholder.style.display = 'none';
          photo.style.display = 'block';
          actionArea.style.display = 'block';
          statusBanner.innerText = "✅ Photo Ready!";

          const filename = 'digicam_' + Date.now() + '.jpg';
          downloadBtn.href = activeObjectUrl;
          downloadBtn.download = filename;
          meta.innerText = 'UXGA 1600x1200 • ' + Math.round(blob.size / 1024) + ' KB • Photo #' + currentPhotoId;

          // Prepare Web Share API if supported in current browser context
          try {
            currentFile = new File([blob], filename, { type: 'image/jpeg' });
            if (navigator.canShare && navigator.canShare({ files: [currentFile] })) {
              shareBtn.style.display = 'flex';
              shareBtn.onclick = async () => {
                try {
                  await navigator.share({
                    files: [currentFile],
                    title: 'DigiCam Photo',
                    text: 'Captured on Psy4ysh DigiCam'
                  });
                } catch (err) {
                  if (err.name !== 'AbortError') console.error('Share error:', err);
                }
              };
            } else {
              shareBtn.style.display = 'none';
            }
          } catch(e) {
            shareBtn.style.display = 'none';
          }

          // Non-blocking auto-download attempt (safely catches sandbox restrictions)
          try {
            const a = document.createElement('a');
            a.href = activeObjectUrl;
            a.download = filename;
            document.body.appendChild(a);
            a.click();
            setTimeout(() => document.body.removeChild(a), 300);
          } catch(e) {}

        } catch (fetchErr) {
          console.error(fetchErr);
          const errMsg = "Error receiving image. Retrying on next frame...";
          promptText.innerText = errMsg;
          statusBanner.innerText = "⚠️ " + errMsg;
        }
      }
    } catch (netErr) {
      connState.innerText = "Reconnecting...";
    } finally {
      isPolling = false;
    }
  }

  setInterval(checkStatus, 1000);
  checkStatus();
</script>
</body>
</html>
)rawliteral";

#endif // WEB_PAGE_H
