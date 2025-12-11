// merged_clock_media_button_idle_auto_switch_fixed.ino
// Merge: clock (Adafruit) + media (U8g2).
// Fix: scrolling bug, robust manual toggle after auto-switch, reset media to defaults in a function.

#include <WiFi.h>
#include <NTPClient.h>
#include <WiFiUdp.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include <U8g2lib.h>

#include <WebServer.h>
#include <ArduinoJson.h>

#include "unifont_custom.h" 

#include <ArduinoOTA.h>
//#include <ElegantOTA.h>


// -------------------- CONFIG --------------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_SDA 19
#define OLED_SCL 20

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

const char* ssid = "YOURWIFI";
const char* password = "YOURPASSWORD";
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 3600, 60000);

WebServer server(80);

const char* otahostname = "ESP32-OTA";
const char* otapassword = "yourpassword";

// Debug
const bool DEBUG_SERIAL = false;

// -------------------- CLOCK (Adafruit) resources --------------------
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSans24pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSerif9pt7b.h>
#include <Fonts/FreeSerif12pt7b.h>
#include <Fonts/FreeSerif18pt7b.h>
#include <Fonts/FreeSerif24pt7b.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMono12pt7b.h>
#include <Fonts/FreeMono18pt7b.h>
#include <Fonts/FreeMono24pt7b.h>

const char* giorniArr[] = {"dom", "lun", "mar", "mer", "gio", "ven", "sab"};
const char* mesiArr[]  = {"gen","feb","mar","apr","mag","giu","lug","ago","set","ott","nov","dic"};

const GFXfont* fontsArr[] = {
  nullptr,
  &FreeSans9pt7b, &FreeSans12pt7b, &FreeSans18pt7b, &FreeSans24pt7b,
  &FreeSansBold9pt7b, &FreeSansBold12pt7b, &FreeSansBold18pt7b, &FreeSansBold24pt7b,
  &FreeSerif9pt7b, &FreeSerif12pt7b, &FreeSerif18pt7b, &FreeSerif24pt7b,
  &FreeMono9pt7b, &FreeMono12pt7b, &FreeMono18pt7b, &FreeMono24pt7b
};
const int numFonts = sizeof(fontsArr) / sizeof(fontsArr[0]);

int fontOrologio = 8;
int fontSecondi  = 0;
int fontData     = 0;
int dimensioneOrologio = 4;
int dimensioneSecondi  = 1;
int dimensioneData     = 1;

int posizioneYOrologio = 35;
int posizioneYSecondi  = 39;
int posizioneYData     = SCREEN_HEIGHT - 12;

unsigned long lastSecond = 0;
unsigned long lastWifiAttempt = 0;
bool wifiConnected = false;
const unsigned long wifiRetryInterval = 400; // ms

// clock state
int hours = 0;
int minutes = 0;
int seconds = 0;
int day = 1;
int month = 1;
int year = 1970;
int dayOfWeek = 4;

// -------------------- MEDIA (U8g2) resources --------------------
const char* HOST_PATH = "/update";
#define USE_CUSTOM_UNIFONT 1
#define CUSTOM_UNIFONT_SYMBOL unifont_custom
#define USE_UNIFONT_FOR_POSITION 1
#define USE_UNIFONT_FOR_VOLUME   1

const int GAP_PIXELS = 30;
const unsigned long SCROLL_STEP_MS = 30;

String mediaTitle = "Nessun titolo";
String mediaArtist = "Nessun artista";
String mediaStatus = "PAUSE";

float serverPosition = 0.0f;
float displayPosition = 0.0f;
float mediaDuration = 0.0f;
unsigned long lastServerMillis = 0;

float mediaProgress = 0.0f;
float targetProgress = 0.0f;
float mediaVolume = 0.0f;
float targetVolume = 0.0f;

int scrollOffsetTitle = 0;
int scrollOffsetArtist = 0;
unsigned long lastScrollMs = 0; 

const int ICON_W = 18;
const int TEXT_AREA_X = ICON_W + 2;
const int TEXT_AREA_RIGHT_MARGIN = 12;
const int TEXT_AREA_W = 128 - TEXT_AREA_X - TEXT_AREA_RIGHT_MARGIN;

const float staleThreshold = 0.5f;
const float seekThreshold  = 1.0f;

// -------------------- MODE / BUTTON --------------------
#define BOOT_BUTTON_PIN 9
enum Mode { MODE_MEDIA=0, MODE_CLOCK=1 };
volatile Mode currentMode = MODE_CLOCK;

// ISR flag + guard
volatile bool buttonISRflag = false;
unsigned long lastToggleMillis = 0;
const unsigned long TOGGLE_GUARD_MS = 200;

// -------------------- IDLE/AUTO-SWITCH --------------------
const unsigned long MEDIA_IDLE_TIMEOUT_MS_DEFAULT = 5UL * 60UL * 1000UL; // 5 minutes
unsigned long MEDIA_IDLE_TIMEOUT_MS = MEDIA_IDLE_TIMEOUT_MS_DEFAULT;

// last time we had media activity (update received, position advanced, volume changed...)
// initialized in setup
unsigned long lastMediaActiveMs = 0;

// to detect movement
float prevDisplayPosition = -1.0f;
const float MOVEMENT_EPS = 0.25f; // seconds movement threshold to consider "activity"

// protect auto-switch briefly after manual toggle so user can open MEDIA without instant re-auto-switch
unsigned long manualProtectUntil = 0;
const unsigned long MANUAL_PROTECT_MS = 2000UL; // 2 seconds

// -------------------- HELPERS --------------------
String formatTimeMMSS(int secondsVal){
  int m = secondsVal / 60;
  int s = secondsVal % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
  return String(buf);
}

// reset media values in one place (reusable)
void resetMediaStateToDefaults(bool keepLastMediaActive = false){
  mediaTitle = "Nessun titolo";
  mediaArtist = "Nessun artista";
  mediaStatus = "PAUSE";
  serverPosition = 0.0f;
  displayPosition = 0.0f;
  mediaDuration = 0.0f;
  lastServerMillis = millis();
  mediaProgress = 0.0f;
  targetProgress = 0.0f;
  mediaVolume = 0.0f;
  targetVolume = 0.0f;
  scrollOffsetTitle = 0;
  scrollOffsetArtist = 0;
  prevDisplayPosition = -1.0f;
  if (!keepLastMediaActive) {
    // caller decides whether to update lastMediaActiveMs
    // normally when auto-switching we won't preserve lastMediaActive
  }
}

// -------------------- CLOCK (Adafruit) functions --------------------
void impostaFontClock(int fontIndex, int dimensione) {
  if (fontIndex >= 0 && fontIndex < numFonts) {
    if (fontsArr[fontIndex] == nullptr) {
      display.setFont(); // default
      display.setTextSize(dimensione);
    } else {
      display.setFont(fontsArr[fontIndex]);
      display.setTextSize(1);
    }
  } else {
    display.setFont();
    display.setTextSize(dimensione);
  }
}

void updateDisplayClock(){
  display.clearDisplay();

  impostaFontClock(fontOrologio, dimensioneOrologio);
  String timeStr = (hours < 10 ? "0" : "") + String(hours) + ":" + (minutes < 10 ? "0" : "") + String(minutes);

  int16_t x1,y1; uint16_t w,h;
  display.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
  int xTime = (SCREEN_WIDTH - w) / 2;
  display.setCursor(xTime, posizioneYOrologio);
  display.setTextColor(WHITE);
  display.print(timeStr);

  // secondi
  impostaFontClock(fontSecondi, dimensioneSecondi);
  String secStr = (seconds < 10 ? "0" : "") + String(seconds);
  display.getTextBounds(secStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(SCREEN_WIDTH - w - 2, posizioneYSecondi);
  display.print(secStr);

  // data
  impostaFontClock(fontData, dimensioneData);
  String dataStr = String(giorniArr[dayOfWeek]) + " " + String(day) + " " + String(mesiArr[month-1]) + " " + String(year);
  display.getTextBounds(dataStr, 0, 0, &x1, &y1, &w, &h);
  int xData = (SCREEN_WIDTH - w) / 2;
  display.setCursor(xData, posizioneYData);
  display.print(dataStr);

  // linea separatrice
  display.drawLine(0, SCREEN_HEIGHT - 15, SCREEN_WIDTH, SCREEN_HEIGHT - 15, WHITE);

  display.display();

  if (DEBUG_SERIAL) {
    Serial.print("[DEBUG] Clock draw: ");
    Serial.print(timeStr); Serial.print(" "); Serial.println(dataStr);
  }
}

// -------------------- MEDIA (U8g2) functions --------------------
void drawStateIcon_u8(){
  const int x = 0, y = 0, w = ICON_W, h = 18;
  u8g2.setDrawColor(0);
  u8g2.drawBox(x,y,w,h);
  u8g2.setDrawColor(1);
  if(mediaStatus == "PLAY"){
    int x1 = x+4, y1 = y+3;
    int x2 = x+4, y2 = y+15;
    int x3 = x+14, y3 = y+9;
    u8g2.drawTriangle(x1,y1,x2,y2,x3,y3);
  } else {
    int barW=3,gap=4,bx=x+4,by=y+3,bh=12;
    u8g2.drawBox(bx,by,barW,bh);
    u8g2.drawBox(bx+barW+gap,by,barW,bh);
  }
}

void drawVolume_u8(){
  int barWidth=6, barHeight=16, barX=128-barWidth, barY=0;
  u8g2.setDrawColor(0);
  u8g2.drawBox(barX,barY,barWidth,barHeight);
  u8g2.setDrawColor(1);
  int volHeight = (int)constrain((mediaVolume/100.0f)*barHeight, 0, barHeight);
  u8g2.drawFrame(barX,barY,barWidth,barHeight);
  u8g2.drawBox(barX, barY + barHeight - volHeight, barWidth, volHeight);
  if(USE_UNIFONT_FOR_VOLUME) u8g2.setFont(CUSTOM_UNIFONT_SYMBOL);
  else u8g2.setFont(u8g2_font_6x10_tr);
  String volStr = String((int)(mediaVolume + 0.5f));
  int numW = u8g2.getUTF8Width(volStr.c_str());
  int xOffset = barX - numW - 2;
  int yOffset = USE_UNIFONT_FOR_VOLUME ? (10 + 6) : ((barHeight - 6) / 2 + 6);
  u8g2.drawUTF8(xOffset, yOffset, volStr.c_str());
}

void drawProgressBar_u8(){
  int barWfilled = (int)((mediaProgress / 100.0f) * 128.0f);
  barWfilled = constrain(barWfilled, 0, 128);
  u8g2.setDrawColor(1);
  u8g2.drawBox(0, 64 - 8, barWfilled, 4);
  u8g2.drawFrame(0, 64 - 8, 128, 4);
  if(USE_UNIFONT_FOR_POSITION) u8g2.setFont(CUSTOM_UNIFONT_SYMBOL);
  else u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawUTF8(0, 64 - 16 + 6, formatTimeMMSS((int)displayPosition).c_str());
  String tot = formatTimeMMSS((int)mediaDuration);
  int len = u8g2.getUTF8Width(tot.c_str());
  u8g2.drawUTF8(128 - len, 64 - 16 + 6, tot.c_str());
}

void drawScrollingUTF8_u8(const String &txt, int baselineY, int areaX, int areaW, int &offset){
  const char* s = txt.c_str();
#if USE_CUSTOM_UNIFONT
  u8g2.setFont(CUSTOM_UNIFONT_SYMBOL);
#else
  u8g2.setFont(u8g2_font_unifont_t_symbols);
#endif
  uint16_t textW = u8g2.getUTF8Width(s);
  if(textW <= (uint16_t)areaW){
    u8g2.drawUTF8(areaX, baselineY, s);
    offset = 0;
  } else {
    int total = textW + GAP_PIXELS;
    int x1 = areaX - offset;
    int x2 = x1 + total;
    u8g2.drawUTF8(x1, baselineY, s);
    u8g2.drawUTF8(x2, baselineY, s);
  }
}

void updateDisplayMedia(){
  u8g2.clearBuffer();
  drawStateIcon_u8();
  drawVolume_u8();
  int titleBaseline = 27;
  int artistBaseline = 41;
  drawScrollingUTF8_u8(mediaTitle, titleBaseline, TEXT_AREA_X, TEXT_AREA_W, scrollOffsetTitle);
  drawScrollingUTF8_u8(mediaArtist, artistBaseline, TEXT_AREA_X, TEXT_AREA_W, scrollOffsetArtist);
  drawProgressBar_u8();
  u8g2.sendBuffer();
}

// scrollTick_u8 is now self-contained: it updates lastScrollMs and offsets
void scrollTick_u8(){
  unsigned long now = millis();
  if(now - lastScrollMs < SCROLL_STEP_MS) return;
  lastScrollMs = now;

#if USE_CUSTOM_UNIFONT
  u8g2.setFont(CUSTOM_UNIFONT_SYMBOL);
#else
  u8g2.setFont(u8g2_font_unifont_t_symbols);
#endif

  uint16_t wTitle = u8g2.getUTF8Width(mediaTitle.c_str());
  if(wTitle > (uint16_t)TEXT_AREA_W){
    int total = wTitle + GAP_PIXELS;
    scrollOffsetTitle++;
    if(scrollOffsetTitle >= total) scrollOffsetTitle = 0;
  } else scrollOffsetTitle = 0;

  uint16_t wArtist = u8g2.getUTF8Width(mediaArtist.c_str());
  if(wArtist > (uint16_t)TEXT_AREA_W){
    int total = wArtist + GAP_PIXELS;
    scrollOffsetArtist++;
    if(scrollOffsetArtist >= total) scrollOffsetArtist = 0;
  } else scrollOffsetArtist = 0;
}

// -------------------- MEDIA HTTP handler --------------------
void handleMediaUpdate(){
  if(DEBUG_SERIAL) Serial.println("[DEBUG] Received /update request");
  if(!server.hasArg("plain")){
    server.send(400, "application/json", "{\"status\":\"no body\"}");
    if(DEBUG_SERIAL) Serial.println("[DEBUG] No body in request");
    return;
  }
  String body = server.arg("plain");
  if(DEBUG_SERIAL){
    Serial.print("[DEBUG] Body: ");
    Serial.println(body);
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, body);
  if(error){
    server.send(400, "application/json", "{\"status\":\"json error\"}");
    if(DEBUG_SERIAL) {
      Serial.print("[DEBUG] JSON parse error: ");
      Serial.println(error.c_str());
    }
    return;
  }

  String newTitle = doc["title"] | "Sconosciuto";
  String newArtist = doc["artist"] | "Sconosciuto";
  String newStatus = doc["status"] | "PAUSE";

  // normalize artists (basic)
  newArtist.replace(";", ",");
  newArtist.replace("|", ",");
  newArtist.replace("/", ",");
  while (newArtist.indexOf(",,") != -1) newArtist.replace(",,", ",");
  newArtist.replace(", ", ",");
  newArtist.replace(" ,", ",");
  newArtist.trim();

  float newPosition = 0.0f;
  float newDuration = 0.0f;
  int newVolume = -1;

  if (!doc["position"].isNull()) {
    newPosition = (float)doc["position"].as<double>();
  }
  if (!doc["duration"].isNull()) {
    newDuration = (float)doc["duration"].as<double>();
  }
  if (!doc["volume"].isNull()) {
    newVolume = doc["volume"].as<int>();
  }

  bool changed = false;
  unsigned long now = millis();

  // any received update is media activity -> reset idle timer
  lastMediaActiveMs = now;

  bool titleArtistChanged = (newTitle != mediaTitle) || (newArtist != mediaArtist);
  bool durationChanged = fabs(newDuration - mediaDuration) > 0.5f;

  if(titleArtistChanged || durationChanged){
    mediaTitle = newTitle;
    mediaArtist = newArtist;
    mediaDuration = newDuration;
    serverPosition = newPosition;
    lastServerMillis = now;
    displayPosition = serverPosition;
    targetProgress = (mediaDuration > 0.0f) ? (displayPosition * 100.0f / mediaDuration) : 0.0f;
    mediaProgress = targetProgress;
    mediaStatus = newStatus;
    if(newVolume >= 0) { targetVolume = (float)newVolume; mediaVolume = targetVolume; }
    scrollOffsetTitle = 0;
    scrollOffsetArtist = 0;
    changed = true;
  } else {
    if(newStatus != mediaStatus){ mediaStatus = newStatus; changed = true; }
    if(newTitle != mediaTitle || newArtist != mediaArtist){ mediaTitle = newTitle; mediaArtist = newArtist; changed = true; }

    if (mediaStatus == "PLAY") {
      float currentEstimate = serverPosition + ((now - lastServerMillis) / 1000.0f);
      float diff = newPosition - currentEstimate;
      if (fabs(diff) >= seekThreshold) {
        serverPosition = newPosition;
        lastServerMillis = now;
        changed = true;
      } else if (newPosition >= currentEstimate - staleThreshold) {
        serverPosition = newPosition;
        lastServerMillis = now;
        changed = true;
      } else {
        serverPosition = currentEstimate;
        lastServerMillis = now;
      }
    } else {
      serverPosition = newPosition;
      lastServerMillis = now;
      changed = true;
    }

    if (fabs(newDuration - mediaDuration) > 0.0001f) {
      mediaDuration = newDuration;
      changed = true;
    }

    // update volume even if title is default (fix missing volume update)
    if(newVolume >= 0 && newVolume != (int)targetVolume){
      targetVolume = (float)newVolume;
      changed = true;
    }
  }

  if(changed && currentMode == MODE_MEDIA) updateDisplayMedia();

  server.send(200, "application/json", "{\"status\":\"ok\"}");
  if(DEBUG_SERIAL) Serial.println("[DEBUG] /update processed, status ok");
}


//----------------------ota setup ------------------------------
void OTASetup(){ 
    ArduinoOTA.setHostname(otahostname);
    ArduinoOTA.setPassword(otapassword);
    ArduinoOTA
      .onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
         type = "sketch";
        else
          type = "filesystem";
       Serial.println("[OTA] Start updating " + type);
      })
      .onEnd([]() {
        Serial.println("\n[OTA] End");
      })
      .onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100)));
      })
      .onError([](ota_error_t error) {
        Serial.printf("[OTA] Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
      });
    
      ArduinoOTA.begin();
      //Serial.println("[OTA] Ready: access via http://" + WiFi.localIP().toString() + "/otaupdate");       //used for elegantOT, not in use right now
}

// -------------------- WiFi/NTP helpers --------------------
void connectToWiFi(){
  if(DEBUG_SERIAL) Serial.println("Tentativo di connessione WiFi...");
  //WiFi.setAutoReconnect(true);  i don't think it is needed??
  //WiFi.persistent(true);        i don't think it is needed??
  WiFi.begin(ssid, password);


  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 1600) {
    delay(200);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    if(DEBUG_SERIAL){
      Serial.println("\Wi-Fi Connected!");
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
    }
    wifiConnected = true;
    timeClient.begin();
  } else {
    if(DEBUG_SERIAL) Serial.println("\nWi-Fi connection failed!");
    wifiConnected = false;
  }
  lastWifiAttempt = millis();
}

// -------------------- Button ISR --------------------
void IRAM_ATTR bootButtonISR(){
  buttonISRflag = true;
}

// -------------------- setup & loop --------------------
void setup(){
  if(DEBUG_SERIAL){
    Serial.begin(115200);
    delay(10);
    Serial.println();
    Serial.println("[DEBUG] setup start");
  } else {
    Serial.begin(115200);
  }

  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  lastToggleMillis = 0;
  buttonISRflag = false;

  Wire.begin(OLED_SDA, OLED_SCL);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)){
    if(DEBUG_SERIAL) Serial.println("[ERROR] SSD1306 (Adafruit) init failed");
  } else {
    display.clearDisplay();
    display.display();
  }

  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.sendBuffer();

  int irq = digitalPinToInterrupt(BOOT_BUTTON_PIN);
  if (irq != NOT_AN_INTERRUPT) {
    attachInterrupt(irq, bootButtonISR, RISING);
    if(DEBUG_SERIAL) {
      Serial.print("[DEBUG] ISR attached to pin ");
      Serial.println(BOOT_BUTTON_PIN);
    }
  } else {
    if(DEBUG_SERIAL) {
      Serial.print("[DEBUG] pin ");
      Serial.print(BOOT_BUTTON_PIN);
      Serial.println(" is not attachable via digitalPinToInterrupt()");
    }
  }

  connectToWiFi();

  server.on(HOST_PATH, HTTP_POST, handleMediaUpdate);
  server.begin();
  if(DEBUG_SERIAL) Serial.println("[DEBUG] Web server started");

  // init media & clock state
  resetMediaStateToDefaults();
  lastMediaActiveMs = millis();
  prevDisplayPosition = -1.0f;

  currentMode = MODE_CLOCK;
  updateDisplayClock();

  if(DEBUG_SERIAL) {
    Serial.println("[DEBUG] Setup complete. Default mode = CLOCK");
    Serial.print("[DEBUG] initial button raw state=");
    Serial.println(digitalRead(BOOT_BUTTON_PIN));
  }

   // call OTA Setup

    OTASetup();
}

void loop(){
  // Always service web server
  server.handleClient();

  ArduinoOTA.handle();

  unsigned long now = millis();

  // handle button ISR flag (debounced guard)
  if (buttonISRflag) {
    if (now - lastToggleMillis > TOGGLE_GUARD_MS) {
      Mode prev = currentMode;
      currentMode = (currentMode == MODE_MEDIA) ? MODE_CLOCK : MODE_MEDIA;
      lastToggleMillis = now;

      // if user manually opens MEDIA, protect from immediate auto-switch:
      if (currentMode == MODE_MEDIA) {
        lastMediaActiveMs = now; // protect for idle timeout
        prevDisplayPosition = -1.0f;
        manualProtectUntil = now + MANUAL_PROTECT_MS; // protect auto-switch briefly
        if (DEBUG_SERIAL) Serial.println("[DEBUG] Manual toggle -> set lastMediaActiveMs to now and protection");
      }

      if (DEBUG_SERIAL) {
        Serial.print("[DEBUG] Button toggled: ");
        Serial.print(prev==MODE_MEDIA ? "MEDIA" : "CLOCK");
        Serial.print(" -> ");
        Serial.println(currentMode==MODE_MEDIA ? "MEDIA" : "CLOCK");
      }

      if (currentMode == MODE_MEDIA) updateDisplayMedia();
      else updateDisplayClock();
    } else {
      if (DEBUG_SERIAL) Serial.println("[DEBUG] ISR guard: ignored");
    }
    buttonISRflag = false;
  }

  // WiFi reconnect attempt (non-blocking)
  if (!wifiConnected && (now - lastWifiAttempt > wifiRetryInterval)) {
    if (DEBUG_SERIAL) Serial.println("[DEBUG] Trying reconnect WiFi...");
    connectToWiFi();
  }

  if (currentMode == MODE_MEDIA) {
    // scroll tick: call the self-contained function (it internally throttles using lastScrollMs)
    scrollTick_u8();

    // update displayPosition estimation
    if(mediaStatus == "PLAY" && mediaDuration > 0.0f){
      float elapsed = (float)(now - lastServerMillis) / 1000.0f;
      displayPosition = serverPosition + elapsed;
      if(displayPosition > mediaDuration) displayPosition = mediaDuration;
    } else {
      displayPosition = serverPosition;
    }

    // detect movement (activity)
    if(prevDisplayPosition < 0.0f) {
      prevDisplayPosition = displayPosition;
    } else {
      if (fabs(displayPosition - prevDisplayPosition) > MOVEMENT_EPS) {
        lastMediaActiveMs = now;
      }
      prevDisplayPosition = displayPosition;
    }

    // easing updates
    targetProgress = (mediaDuration > 0.0f) ? (displayPosition * 100.0f / mediaDuration) : 0.0f;
    const float easing = 0.21f;
    mediaProgress += (targetProgress - mediaProgress) * easing;
    mediaVolume  += (targetVolume - mediaVolume) * easing;

    // throttle actual redraw to reduce CPU
    static unsigned long lastMediaDrawMs = 0;
    const unsigned long MEDIA_DRAW_MS = 16; // ~60 FPS
    if (now - lastMediaDrawMs >= MEDIA_DRAW_MS) {
      lastMediaDrawMs = now;
      updateDisplayMedia();
    }

    // AUTO-SWITCH: guard manual protection, then if no activity for configured timeout -> switch to clock
    if (now < manualProtectUntil) {
      // still in protection window after manual toggle; do NOT auto-switch
    } else {
      if ((now - lastMediaActiveMs) >= MEDIA_IDLE_TIMEOUT_MS) {
        if (currentMode == MODE_MEDIA) {
          currentMode = MODE_CLOCK;
          // reset media state to defaults (as requested)
          resetMediaStateToDefaults();
          // IMPORTANT: set lastMediaActiveMs to now so we don't immediately flip back
          lastMediaActiveMs = now;
          if (DEBUG_SERIAL) {
            Serial.println("[DEBUG] Auto-switch to CLOCK due to media idle. Media state reset to defaults.");
          }
          updateDisplayClock();
        }
      }
    }

  } else {
    // CLOCK mode - update at most once per second
    static unsigned long lastClockDrawMs = 0;
    if (wifiConnected) {
      timeClient.update(); // lightweight
      if (now - lastClockDrawMs >= 1000) {
        lastClockDrawMs = now;
        hours = timeClient.getHours();
        minutes = timeClient.getMinutes();
        seconds = timeClient.getSeconds();
        time_t epochTime = timeClient.getEpochTime();
        struct tm *ptm = gmtime((time_t *)&epochTime);
        day = ptm->tm_mday; month = ptm->tm_mon + 1; year = ptm->tm_year + 1900; dayOfWeek = ptm->tm_wday;
        updateDisplayClock();
      }
    } else {
      if (now - lastSecond >= 1000) {
        lastSecond = now;
        seconds++;
        if (seconds >= 60) { seconds = 0; minutes++; if (minutes >= 60) { minutes = 0; hours = (hours + 1) % 24; } }
        updateDisplayClock();
      }
    }
  }

  // yield to WiFi stack / background tasks (ESP32 / Arduino): keeps WiFi & RTOS happy
  // On ESP32 this is the recommended lightweight way to let background tasks run.
  yield();
}
