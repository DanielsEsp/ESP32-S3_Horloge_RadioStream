/*
 * ESP32-S3 - Horloge (mode par défaut) + Radio Stream laut.fm (bascule 2 doigts)
 * --------------------------------------------------------------------------
 *  - Démarrage : mode HORLOGE, écran en paysage (480x320), cadran centré
 *    horizontalement sur les 480px, plus de date/heure en texte (uniquement
 *    la partie graphique : aiguilles + anneau de 60 segments).
 *  - Toucher l'écran avec 2 doigts (n'importe où) : bascule HORLOGE <-> RADIO.
 *  - En mode HORLOGE : PCM5102 muet (XSMT bas), stream radio arrêté proprement.
 *  - En mode RADIO   : PCM5102 démuté (XSMT haut), interface laut.fm inchangée
 *    (boutons stations / play-pause / stop / volume), tâche laut.fm sur le
 *    coeur 0, audio.loop() sur le coeur 1 (loop() principal).
 *
 *  Bibliothèques requises : TFT_eSPI, U8g2_for_TFT_eSPI, TJpg_Decoder,
 *  ESP32-audioI2S (Audio.h), FT6336U, ArduinoJson, SPIFFS.
 *  Configuration TFT_eSPI (User_Setup.h) : gérée par toi, comme convenu.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <sys/time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Audio.h"
#include <TFT_eSPI.h>
#include "U8g2_for_TFT_eSPI.h"
#include <TJpg_Decoder.h>
#include <FT6336U.h>
#include <FS.h>
#include <SPIFFS.h>

// =====================================================
// WIFI / NTP
// =====================================================
const char* ssid     = "Bbox-E295749E-2,4GHz";
const char* password = "Mb&2427242711";
const char* TZ_INFO  = "CET-1CEST,M3.5.0,M10.5.0/3";   // Europe/Paris

// =====================================================
// I2S -> bus partagé PCM5102 (+ MAX98357 en mode radio)
// =====================================================
#define I2S_BCLK  5
#define I2S_LRC   6
#define I2S_DOUT  4

// Mute matériel du PCM5102 (XSMT). Bas = muet, Haut = actif.
// A câbler uniquement sur XSMT du PCM5102 (montage horloge : pas de MAX98357).
// IMPORTANT : sur ESP32-S3, les GPIO 26-32 sont réservés au bus SPI flash
// (et 33-37 à la PSRAM Octal selon le module) -> ne jamais les utiliser en
// GPIO générique, sous peine de crash (LoadProhibited) notamment sous charge
// mémoire comme l'init WiFi. GPIO21 est libre et sans fonction spéciale.
#define MUTE_PIN  21

// =====================================================
// MODE APPLICATION (HORLOGE / RADIO)
// =====================================================
enum AppMode { MODE_CLOCK, MODE_RADIO };
     AppMode currentMode = MODE_CLOCK;   // <-- démarrage en mode HORLOGE

// Détection du toucher à 2 doigts : anti-rebond + cooldown après bascule,
// pour ne pas re-basculer immédiatement sur un résidu de lecture tactile.
const unsigned long MODE_SWITCH_COOLDOWN_MS = 800;
      unsigned long lastModeSwitch = 0;
                int prevTouchCount = 0;

// =====================================================
// STATION LAUT.FM
// =====================================================
const String stationName[7] = { "", "lautstark", "70s", "disco", "dancetime", "eurodance", "apfelstadtradio"};
      String stationTxt[7]  = { "", "LAUTSTARK", "70s", "DISCO", "DANCE TIME", "EURODANCE", "APFELSTADT RADIO"};
      String remplissage = "                              ";

volatile int  currentStationIndex = 1;
volatile bool forceUpdate = true;
volatile bool isStopped = true;   // true tant qu'on n'est pas passé en mode radio

    int  radioNbr, radioIndex, oldRadioIndex;
    int  screenPointX, oldScreenPointX, screenPointY, oldScreenPointY;
int16_t  tx, ty;
int16_t  lastX = -1, lastY = -1;

// =====================================================
// AUDIO / TFT / TOUCH / SPIFFS
// =====================================================
Audio audio;
int volumeIndex = 8;

   TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);   // sprite bande, réutilisé pour l'horloge
#define BLEU_STATION 0x0DFE
U8g2_for_TFT_eSPI u8f;

#define TP_SDA   8
#define TP_SCL   9
#define TP_RST  18
#define TP_INT  17
FT6336U ft6336u(TP_SDA, TP_SCL, TP_RST, TP_INT);
FT6336U_TouchPointType touchData;
        int x, y;

const char* NO_IMAGE_PATH =    "/No_Image.jpg";
const char* NO_IMAGE_SENTINEL = "__NO_IMAGE__";
        int imgX = 0;
        int imgY = 0;
     String image = "";

// =====================================================
// TACHE LAUT.FM (COEUR 0)
// =====================================================
const unsigned long updateInterval = 15000;
             String lastTitle    = "";
             String lastThumbUrl = "";
TaskHandle_t lautfmTaskHandle = NULL;

// =====================================================
// DEFILEMENT DES TEXTES
// =====================================================
struct ScrollTextState {
    String text;
    int width;
    int offset;
    int direction;
    unsigned long pauseUntil;
    unsigned long lastStep;
    int lastRenderedOffset;
};
ScrollTextState scrollArtist = {"", 0, 0, 1, 0, 0, -1};
ScrollTextState scrollTitle  = {"", 0, 0, 1, 0, 0, -1};
ScrollTextState scrollAlbum  = {"", 0, 0, 1, 0, 0, -1};

          const int SCROLL_WIDTH = 400;
          const int SCROLL_SPEED = 2;
const unsigned long SCROLL_STEP = 35;
const unsigned long SCROLL_PAUSE = 1200;


// =====================================================
// ================  PARTIE RADIO  ====================
// =====================================================

void audio_info(const char* info) {
  
     Serial.print("[AUDIO] ");
     Serial.println(info);
     
}


bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  
     if (y >= tft.height()) return 0;
         tft.pushImage(x, y, w, h, bitmap);
         return 1;
         
}


void image_display() {
  
     String path = "/" + image + ".jpg";
     uint16_t jpgW = 0, jpgH = 0;
     TJpgDec.getFsJpgSize(&jpgW, &jpgH, path.c_str());
     tft.fillRect(imgX, imgY, jpgW, jpgH, TFT_BLACK);
     TJpgDec.drawFsJpg(imgX, imgY, path.c_str());
     
}


void displayNoImage() {
  
     if (lastThumbUrl == NO_IMAGE_SENTINEL) return;

     Serial.println();
     Serial.println("Aucune pochette disponible, affichage de l'image par defaut.");

     if (!SPIFFS.exists(NO_IMAGE_PATH)) {
         Serial.print("ERREUR : fichier introuvable dans SPIFFS : ");
         Serial.println(NO_IMAGE_PATH);
         return;
        }

     uint16_t jpgW = 0, jpgH = 0;
     TJpgDec.getFsJpgSize(&jpgW, &jpgH, NO_IMAGE_PATH);
     tft.fillRect(10, 15, jpgW, jpgH, TFT_BLACK);
     TJpgDec.drawFsJpg(10, 15, NO_IMAGE_PATH);
     lastThumbUrl = NO_IMAGE_SENTINEL;
     
}


void downloadAndDisplayThumb(const String &thumbUrl) {
  
     if (thumbUrl.length() == 0) { displayNoImage(); return; }
     if (thumbUrl == lastThumbUrl) { return; }

     Serial.println();
     Serial.println("Telechargement de l'image thumb...");
     Serial.println(thumbUrl);

     HTTPClient http;
     http.begin(thumbUrl);
     http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

     int httpCode = http.GET();
     if (httpCode != HTTP_CODE_OK) {
         Serial.print("Erreur HTTP image : ");
         Serial.println(httpCode);
         http.end();
         displayNoImage();
         return;
        }

     int len = http.getSize();
     if (len <= 0) {
         Serial.println("Taille image invalide.");
         http.end();
         displayNoImage();
         return;
        }

     uint8_t* jpgBuffer = (uint8_t*) heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
     if (!jpgBuffer) { jpgBuffer = (uint8_t*) malloc(len); }
     if (!jpgBuffer) {
         Serial.println("Erreur allocation memoire image.");
         http.end();
         displayNoImage();
         return;
        }

     WiFiClient* stream = http.getStreamPtr();
     int bytesRead = 0;
     unsigned long lastByteTime = millis();
     while (bytesRead < len && (millis() - lastByteTime) < 10000) {
            if (stream->available()) {
                int n = stream->readBytes(jpgBuffer + bytesRead, len - bytesRead);
                bytesRead += n;
                lastByteTime = millis();
               } else {
                vTaskDelay(pdMS_TO_TICKS(5));
               }
           }
     http.end();

     if (bytesRead != len) {
         Serial.println("Telechargement image incomplet.");
         free(jpgBuffer);
         displayNoImage();
         return;
        }

     uint16_t jpgW = 0, jpgH = 0;
     TJpgDec.getJpgSize(&jpgW, &jpgH, jpgBuffer, len);
     if (jpgW == 0 || jpgH == 0) {
         Serial.println("Echec decodage JPEG.");
         free(jpgBuffer);
         displayNoImage();
         return;
        }

     tft.fillRect(10, 15, jpgW, jpgH, TFT_BLACK);
     TJpgDec.drawJpg(10, 15, jpgBuffer, len);
     free(jpgBuffer);
     lastThumbUrl = thumbUrl;
     
}


void setScrollText(ScrollTextState &state, const String &text) {
  
     state.text = text;
     state.width = u8f.getUTF8Width(text.c_str());
     state.offset = 0;
     state.direction = 1;
     state.lastStep = millis();
     state.pauseUntil = millis() + SCROLL_PAUSE;
     state.lastRenderedOffset = -1;
}


void drawScrollingText(ScrollTextState &state, int cursorY, int topY) {
  
     if (isStopped) return;

     bool mustDraw = (state.lastRenderedOffset != state.offset);

     if (state.width > SCROLL_WIDTH &&
         millis() >= state.pauseUntil &&
         millis() - state.lastStep >= SCROLL_STEP) {

         state.lastStep = millis();
         int maxOffset = state.width - SCROLL_WIDTH;
         state.offset += state.direction * SCROLL_SPEED;

         if (state.offset >= maxOffset) {
             state.offset = maxOffset;
             state.direction = -1;
             state.pauseUntil = millis() + SCROLL_PAUSE;
            }
         if (state.offset <= 0) {
             state.offset = 0;
             state.direction = 1;
             state.pauseUntil = millis() + SCROLL_PAUSE;
            }
         mustDraw = true;
        }

     if (!mustDraw) return;

     tft.fillRect(0, topY, SCROLL_WIDTH, 30, TFT_BLACK);
     u8f.setForegroundColor(TFT_LIGHTGREY);
     tft.setViewport(0, topY, SCROLL_WIDTH, 30);

     if (state.width <= SCROLL_WIDTH) {
         int posX = (SCROLL_WIDTH - state.width) / 2;
         u8f.setCursor(posX, cursorY - topY);
        } else {
         u8f.setCursor(-state.offset, cursorY - topY);
        }
     u8f.print(state.text);
     tft.resetViewport();
     state.lastRenderedOffset = state.offset;
     
}


void updateScrollingTexts() {
  
     if (isStopped) return;
     drawScrollingText(scrollArtist, 110, 90);
     drawScrollingText(scrollTitle,  170, 150);
     drawScrollingText(scrollAlbum,  225, 205);
     
}


void getLautFMInfo() {
  
     int stationIdx = currentStationIndex;
     String currentSongURL = "https://api.laut.fm/station/" + stationName[stationIdx] + "/current_song";

     HTTPClient http;
     http.begin(currentSongURL);
     http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

     int httpCode = http.GET();
     if (httpCode == HTTP_CODE_OK) {
         String payload = http.getString();
         JsonDocument doc;
         DeserializationError error = deserializeJson(doc, payload);

        if (error) {
            Serial.print("Erreur JSON : ");
            Serial.println(error.c_str());
            http.end();
            return;
           }

       const char* title = doc["title"];
       String currentTitle = title ? String(title) : "";
       String titleTft = currentTitle;

       if (currentTitle.length() > 0 && currentTitle != lastTitle) {
           lastTitle = currentTitle;
           String artistTft = doc["artist"]["name"];
           String albumTft = doc["album"];
           const char* thumb = doc["artist"]["thumb"];

           tft.fillRect(0, 0, 400, 70, TFT_BLACK);
           int longTxt = stationTxt[radioIndex].length();
           int posTxt = (27 - longTxt) / 2;
           String decal = remplissage.substring(0, posTxt);
           u8f.setFont(u8g2_font_profont29_tf);
           u8f.setForegroundColor(BLEU_STATION);
           u8f.setCursor(0, 45); u8f.print(decal + stationTxt[radioIndex]);

           u8f.setFont(u8g2_font_profont22_tf);
           tft.fillRect(0, 55, 400, 180, TFT_BLACK);
           u8f.setForegroundColor(TFT_DARKGREY);
           u8f.setCursor(0, 85); u8f.print("____________  Artiste  __________");
           u8f.setCursor(0, 145); u8f.print("_____________  Titre  ___________");
           u8f.setCursor(0, 205); u8f.print("_____________  Album  ___________");

           setScrollText(scrollArtist, artistTft);
           setScrollText(scrollTitle,  titleTft);
           setScrollText(scrollAlbum,  albumTft);
           updateScrollingTexts();

           if (thumb && strlen(thumb) > 0) {
               downloadAndDisplayThumb(String(thumb));
              } else {
               displayNoImage();
              }
          }
     } else {
      Serial.print("Erreur HTTP laut.fm : ");
      Serial.println(httpCode);
     }
     http.end();
     
}


void lautfmTask(void* parameter) {
  
     for (;;) {  
       // Ne sollicite laut.fm que si on est réellement en mode radio actif
          if (currentMode == MODE_RADIO && WiFi.status() == WL_CONNECTED && !isStopped) {
              getLautFMInfo();
              }

          forceUpdate = false;
          unsigned long waited = 0;
          while (waited < updateInterval && !forceUpdate) {
                 if (currentMode == MODE_RADIO) updateScrollingTexts(); {
                     vTaskDelay(pdMS_TO_TICKS(35));
                     waited += 35;
                    }
                }
         }
         
}


void switchStation(int newIndex) {
  
     String newRadioURL = "https://stream.laut.fm/" + stationName[newIndex];
     Serial.println();
     Serial.print("Changement de station -> ");
     Serial.println(stationName[newIndex]);

     audio.connecttohost(newRadioURL.c_str());
     isStopped = false;
     currentStationIndex = newIndex;
     lastTitle    = "";
     lastThumbUrl = "";
     forceUpdate = true;
     
}


// Dessine l'interface radio complète (icônes + station en surbrillance).
// Appelée uniquement en entrant en mode radio (plus au boot, qui démarre
// désormais sur l'horloge).
void drawRadioUI() {
  
     tft.fillScreen(TFT_BLACK);
     imgX = 400; imgY = 0;   image = "pause";      image_display();
     imgX = 400; imgY = 80;  image = "volDown";    image_display();
     imgX = 400; imgY = 160; image = "volUp";      image_display();
     imgX =   0; imgY = 240; image = "lautstarkC"; image_display();
     imgX =  80; imgY = 240; image = "70sB";       image_display();
     imgX = 160; imgY = 240; image = "discoB";     image_display();
     imgX = 240; imgY = 240; image = "dancetimeB"; image_display();
     imgX = 320; imgY = 240; image = "eurodanceB"; image_display();
     imgX = 400; imgY = 240; image = "stop";       image_display();

     oldRadioIndex   = 1;
     radioIndex      = 1;
     oldScreenPointX = 0;
     oldScreenPointY = 240;

     displayNoImage();
     
}


// =====================================================
// ================  PARTIE HORLOGE  ===================
// =====================================================
// Géométrie : écran 480x320 (paysage). Le cadran (320x320) est centré
// horizontalement -> décalage de 80px de chaque côté, aucune bande de
// texte (date/heure supprimée), le cadran occupe toute la hauteur.
const int FACE     = 320;
const int FACE_X   = 80;     // (480 - 320) / 2 : centrage horizontal
const int FACE_Y   = 0;      // occupe toute la hauteur, pas de bande de texte
const int CX       = 160;    // centre local du sprite (inchangé)
const int CY       = 160;
const int BAND_H   = 40;
const int NBANDS   = FACE / BAND_H;

const float R_FACE  = 158.0f;
const float R_OUT   = 141.0f;
const float R_IN    = 112.0f;
const float R_INNER = 78.0f;

const int TRAIL_N = 26;
const float TRAIL_STEP = 2.5f;

const bool SMOOTH_SECONDS = true;
const uint16_t FRAME_MS = 40;

uint16_t colBlack, colFace, colBorder, colOff, colOn, colOnHi, colHour, colMin, colInner, colHubDark;
uint16_t trailCol[TRAIL_N];

int16_t segX[60][4], segY[60][4];
int16_t segMinY[60], segMaxY[60];

     int yOff = 0;
     int lastMinuteClock = -1;
uint32_t lastFrameClock = 0;

uint16_t mix565(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2, float t) {  
                return tft.color565(r1 + (r2 - r1) * t, g1 + (g2 - g1) * t, b1 + (b2 - b1) * t);
                }
inline float px(float aDeg, float r) { return CX + r * sinf(aDeg * DEG_TO_RAD); }
inline float py(float aDeg, float r) { return CY - r * cosf(aDeg * DEG_TO_RAD); }


void tri(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c) {
  
         spr.fillTriangle((int)(x0 + 0.5f), (int)(y0 + 0.5f) - yOff,
                          (int)(x1 + 0.5f), (int)(y1 + 0.5f) - yOff,
                          (int)(x2 + 0.5f), (int)(y2 + 0.5f) - yOff, c);
                          
}


void thickLine(float x0, float y0, float x1, float y1, float w, uint16_t c) {
  
               float dx = x1 - x0, dy = y1 - y0;
               float len = sqrtf(dx * dx + dy * dy);
               if (len < 0.01f) return;
               float nx = -dy / len * w * 0.5f;
               float ny =  dx / len * w * 0.5f;
               tri(x0 + nx, y0 + ny, x0 - nx, y0 - ny, x1 + nx, y1 + ny, c);
               tri(x0 - nx, y0 - ny, x1 - nx, y1 - ny, x1 + nx, y1 + ny, c);
               
}


void hand(float aDeg, float tail, float length, float width, uint16_t c) {
  
          thickLine(px(aDeg, -tail), py(aDeg, -tail), px(aDeg, length), py(aDeg, length), width, c);
          
}


void initColors() {
  
     colBlack   = tft.color565(0, 0, 0);
     colFace    = tft.color565(14, 15, 19);
     colBorder  = tft.color565(45, 46, 50);
     colOff     = tft.color565(32, 33, 36);
     colOn      = tft.color565(0, 190, 210);
     colOnHi    = tft.color565(60, 235, 255);
     colHour    = tft.color565(205, 205, 208);
     colMin     = tft.color565(160, 160, 165);
     colInner   = tft.color565(38, 40, 45);
     colHubDark = tft.color565(6, 16, 20);

     for (int k = 0; k < TRAIL_N; k++) {
          float t = 0.42f * powf(1.0f - (float)k / TRAIL_N, 1.7f);
          trailCol[k] = mix565(14, 15, 19, 0, 190, 210, t);
          }
}


void initSegments() {
  
     for (int i = 0; i < 60; i++) {
          float a0 = i * 6.0f + 1.2f;
          float a1 = (i + 1) * 6.0f - 1.2f;
          segX[i][0] = px(a0, R_OUT); segY[i][0] = py(a0, R_OUT);
          segX[i][1] = px(a1, R_OUT); segY[i][1] = py(a1, R_OUT);
          segX[i][2] = px(a1, R_IN);  segY[i][2] = py(a1, R_IN);
          segX[i][3] = px(a0, R_IN);  segY[i][3] = py(a0, R_IN);
          int mn = 9999, mx = -9999;
          for (int k = 0; k < 4; k++) {
               if (segY[i][k] < mn) mn = segY[i][k];
               if (segY[i][k] > mx) mx = segY[i][k];
              }
          segMinY[i] = mn - 1;
          segMaxY[i] = mx + 1;
         }
         
}


void drawBand(int secInt, float secAngle, float minAngle, float hourAngle) {
  
              spr.fillSprite(colBlack);
              spr.fillCircle(CX, CY - yOff, (int)R_FACE, colFace);
              spr.drawCircle(CX, CY - yOff, (int)R_FACE + 1, colBorder);

              for (int i = 0; i < 60; i++) {
                   if (segMaxY[i] < yOff || segMinY[i] >= yOff + BAND_H) continue;
                   uint16_t c = colOff;
                   if (i <= secInt) c = (i == secInt) ? colOnHi : colOn;
                   tri(segX[i][0], segY[i][0], segX[i][1], segY[i][1], segX[i][2], segY[i][2], c);
                   tri(segX[i][0], segY[i][0], segX[i][2], segY[i][2], segX[i][3], segY[i][3], c);
                  }

              const float TR = R_IN - 3.0f;
              for (int k = 0; k < TRAIL_N; k++) {
                   float a0 = secAngle - k * TRAIL_STEP;
                   float a1 = a0 - TRAIL_STEP - 0.3f;
                   tri(CX, CY, px(a0, TR), py(a0, TR), px(a1, TR), py(a1, TR), trailCol[k]);
                  }

              spr.drawCircle(CX, CY - yOff, (int)R_INNER, colInner);
              hand(hourAngle, 18, 88, 7, colHour);
              hand(minAngle,  20, 122, 5, colMin);
              hand(secAngle, 30, 132, 2.2f, colOnHi);
              spr.fillCircle(CX, CY - yOff, 8, colOn);
              spr.fillCircle(CX, CY - yOff, 4, colHubDark);
              
}


void setFallbackTime() {
  
     static const char* mois[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
     char mon[4] = "Jan";
     int day = 1, year = 2026, h = 0, m = 0, sec = 0;
     sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
     sscanf(__TIME__, "%d:%d:%d", &h, &m, &sec);
     int mi = 0;
     for (int i = 0; i < 12; i++) if (strncmp(mon, mois[i], 3) == 0) mi = i;

     setenv("TZ", TZ_INFO, 1);
     tzset();
     struct tm tmv = {};
     tmv.tm_year = year - 1900;
     tmv.tm_mon = mi;
     tmv.tm_mday = day;
     tmv.tm_hour = h;
     tmv.tm_min = m;
     tmv.tm_sec = sec;
     tmv.tm_isdst = -1;
     struct timeval tv = { mktime(&tmv), 0 };
     settimeofday(&tv, nullptr);
     
}


// Synchronise l'heure via NTP. Le WiFi reste connecté ensuite (nécessaire
// pour le mode radio), contrairement à la version horloge seule d'origine.
void syncTime() {
  
     tft.fillScreen(TFT_BLACK);
     tft.setTextColor(TFT_WHITE, TFT_BLACK);
     tft.drawCentreString("Synchronisation NTP...", 240, 150, 4);

     configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "fr.pool.ntp.org");
     struct tm t;
     if (!getLocalTime(&t, 15000)) {
         tft.fillScreen(TFT_BLACK);
         tft.setTextColor(tft.color565(0, 190, 210), TFT_BLACK);
         tft.drawCentreString("NTP indisponible", 240, 130, 4);
         tft.drawCentreString("Heure de compilation", 240, 170, 4);
         setFallbackTime();
         delay(2000);
        }
     tft.fillScreen(TFT_BLACK);
     
}


// =====================================================
// ============  BASCULE ENTRE LES 2 MODES  ============
// =====================================================
void enterClockMode() {
  
     if (currentMode == MODE_CLOCK) return;

  // Mute matériel immédiat (avant toute coupure logicielle) pour éviter le "pop"
     digitalWrite(MUTE_PIN, LOW);
     delay(20);
     if (audio.isRunning()) audio.stopSong();
     isStopped = true;

     currentMode = MODE_CLOCK;
     tft.fillScreen(TFT_BLACK);
     lastMinuteClock = -1;
     lastFrameClock = 0;

     Serial.println("--> Mode HORLOGE");
     
}


void enterRadioMode() {
  
     if (currentMode == MODE_RADIO) return;

     currentMode = MODE_RADIO;
     drawRadioUI();

     isStopped = false;
     lastTitle = "";
     lastThumbUrl = "";
     String url = "https://stream.laut.fm/" + stationName[currentStationIndex];
     audio.connecttohost(url.c_str());
     forceUpdate = true;

     delay(300);                     // laisse le décodeur/flux se stabiliser
     digitalWrite(MUTE_PIN, HIGH);   // démute une fois la lecture réellement lancée
     Serial.println("--> Mode RADIO");
     
}


// =====================================================
// SETUP
// =====================================================
void setup() {
  
     Serial.begin(115200); delay(100);
     Serial.println();
     Serial.println("========================================");
     Serial.println("   ESP32-S3 HORLOGE + RADIO (2 doigts)");
     Serial.println("========================================");

  // --- Mute PCM5102 dès le boot : on démarre en mode horloge, silencieux ---
     pinMode(MUTE_PIN, OUTPUT);
     digitalWrite(MUTE_PIN, LOW);

  // --- SPIFFS ---
     if (!SPIFFS.begin(true)) {
         Serial.println("ERREUR : montage SPIFFS impossible !");
        }

  // --- WiFi (nécessaire pour NTP et pour le stream radio) ---
     Serial.println("Connexion WiFi...");
     WiFi.mode(WIFI_STA);
     WiFi.begin(ssid, password);
     uint32_t t0 = millis();
     while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
     if (WiFi.status() == WL_CONNECTED) {
         Serial.print("WiFi connecte, IP : ");
         Serial.println(WiFi.localIP());
        } else {
         Serial.println("WiFi indisponible, poursuite en mode degrade.");
        }

  // --- Écran / décodeur JPEG / texte U8g2 ---
     tft.init();
     tft.setRotation(1);            // paysage 480x320
     tft.fillScreen(TFT_BLACK);
     u8f.begin(tft);
     u8f.setForegroundColor(TFT_LIGHTGREY);
     u8f.setFont(u8g2_font_profont22_tf);
     TJpgDec.setJpgScale(1);
     TJpgDec.setSwapBytes(true);
     TJpgDec.setCallback(tft_output);

  // --- Sprite bande pour le rendu de l'horloge ---
     spr.setColorDepth(16);
     if (!spr.createSprite(FACE, BAND_H)) {
         Serial.println("Erreur : creation du sprite impossible");
         while (true) delay(1000);
        }
     initColors();
     initSegments();

  // --- Heure (NTP, WiFi laissé connecté pour la radio) ---
     syncTime();

  // --- Tactile ---
     ft6336u.begin();

  // --- I2S / Audio (pinout et volume prêts, pas de connexion tant qu'on est en horloge) ---
     audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
     audio.setVolume(volumeIndex);

  // --- Tâche laut.fm sur le coeur 0 (inactive tant que currentMode != MODE_RADIO) ---
     xTaskCreatePinnedToCore(
                             lautfmTask,
                             "LautFM_Task",
                             12288,
                             NULL,
                             1,
                             &lautfmTaskHandle,
                             0
                            );

  // --- Démarrage : mode horloge ---
     currentMode = MODE_CLOCK;
     tft.fillScreen(TFT_BLACK);
     
}


// =====================================================
// LOOP
// =====================================================
void loop() {

  // ------------------------------------------------
  // Lecture tactile (une fois par tour de boucle)
  // ------------------------------------------------
     touchData = ft6336u.scan();
     int touchCount = touchData.touch_count;
     if (touchCount > 0) {
         x = touchData.tp[0].x;
         y = touchData.tp[0].y;
        }

  // ------------------------------------------------
  // Détection du geste 2 doigts (front montant + cooldown)
  // ------------------------------------------------
     bool twoFingerEdge = (touchCount == 2 && prevTouchCount != 2);
     prevTouchCount = touchCount;

     if (twoFingerEdge && (millis() - lastModeSwitch > MODE_SWITCH_COOLDOWN_MS)) {
         lastModeSwitch = millis();
         if (currentMode == MODE_CLOCK) {
             enterRadioMode();
            } else {
             enterClockMode();
            }
        }

  // ------------------------------------------------
  // Comportement selon le mode courant
  // ------------------------------------------------
     if (currentMode == MODE_RADIO) {

         audio.loop();
         delay(1);

      // Les boutons de l'interface radio ne réagissent qu'au toucher simple,
      // pour ne jamais interférer avec le geste 2 doigts de bascule.
         if (touchCount == 1) {

         if ((x > 1 && x < 60) && (y > 1 && y < 60)) {
             imgX = oldScreenPointX; imgY = oldScreenPointY; image = stationName[oldRadioIndex] + "B"; image_display();
             radioIndex = 1; x = 0, y = 0;
             imgX = 0; imgY = 240; image = stationName[radioIndex] + "C"; image_display();
             oldScreenPointX = imgX; oldScreenPointY = imgY; oldRadioIndex = radioIndex;
             switchStation(radioIndex);
             imgX = 400; imgY = 0; image = "pause"; image_display();
            }
         if ((x > 1 && x < 60) && (y > 70 && y < 150)) {
             imgX = oldScreenPointX; imgY = oldScreenPointY; image = stationName[oldRadioIndex] + "B"; image_display();
             radioIndex = 2; x = 0, y = 0;
             imgX = 81; imgY = 240; image = stationName[radioIndex] + "C"; image_display();
             oldScreenPointX = imgX; oldScreenPointY = imgY; oldRadioIndex = radioIndex;
             switchStation(radioIndex);
             imgX = 400; imgY = 0; image = "pause"; image_display();
            }
         if ((x > 1 && x < 60) && (y > 150 && y < 230)) {
             imgX = oldScreenPointX; imgY = oldScreenPointY; image = stationName[oldRadioIndex] + "B"; image_display();
             radioIndex = 3; x = 0, y = 0;
             imgX = 161; imgY = 240; image = stationName[radioIndex] + "C"; image_display();
             oldScreenPointX = imgX; oldScreenPointY = imgY; oldRadioIndex = radioIndex;
             switchStation(radioIndex);
             imgX = 400; imgY = 0; image = "pause"; image_display();
            }
         if ((x > 1 && x < 60) && (y > 240 && y < 320)) {
             imgX = oldScreenPointX; imgY = oldScreenPointY; image = stationName[oldRadioIndex] + "B"; image_display();
             radioIndex = 4; x = 0, y = 0;
             imgX = 241; imgY = 240; image = stationName[radioIndex] + "C"; image_display();
             oldScreenPointX = imgX; oldScreenPointY = imgY; oldRadioIndex = radioIndex;
             switchStation(radioIndex);
             imgX = 400; imgY = 0; image = "pause"; image_display();
            }
         if ((x > 1 && x < 60) && (y > 320 && y < 400)) {
             imgX = oldScreenPointX; imgY = oldScreenPointY; image = stationName[oldRadioIndex] + "B"; image_display();
             radioIndex = 5; x = 0, y = 0;
             imgX = 321; imgY = 240; image = stationName[radioIndex] + "C"; image_display();
             oldScreenPointX = imgX; oldScreenPointY = imgY; oldRadioIndex = radioIndex;
             switchStation(radioIndex);
             imgX = 400; imgY = 0; image = "pause"; image_display();
            }

         if (x > 250 && y > 400) {
             bool enCours = audio.isRunning();
             if (enCours == false) {
                 imgX = 400; imgY = 0; image = "pause"; image_display(); x = 0, y = 0;
                 audio.pauseResume(); Serial.println("Play");
                } else {
                 imgX = 400; imgY = 0; image = "play"; image_display(); x = 0, y = 0;
                 audio.pauseResume(); Serial.println("Pause");
                }
             delay(200);
            }
    
         if ((x > 1 && x < 60) && (y > 400 && y < 480)) {
             bool enCours = audio.isRunning();
             if (enCours == true) {
                 audio.stopSong();
                 isStopped = true;
                 imgX = 400; imgY = 0; image = "pause"; image_display();
                 imgX =  0; imgY = 240; image = "lautstarkB"; image_display();
                 imgX = 80; imgY = 240; image = "70sB"; image_display();
                 imgX = 160; imgY = 240; image = "discoB"; image_display();
                 imgX = 240; imgY = 240; image = "dancetimeB"; image_display();
                 imgX = 320; imgY = 240; image = "eurodanceB"; image_display();
                 x = 0, y = 0; delay(2000);
                 tft.fillRect(0, 0, 400, 240, TFT_BLACK);
                 imgX = 30; imgY = 50; image = "radio"; image_display();
                 Serial.println("Stop");
                }
             delay(100);
            }

         if ((x > 160 && x < 210) && (y > 400)) {
             volumeIndex--; audio.setVolume(volumeIndex);
             x = 0, y = 0; Serial.println("Volume Down");
            }
         if ((x > 90 && x < 130) && (y > 400)) {
             volumeIndex++; audio.setVolume(volumeIndex);
             x = 0, y = 0; Serial.println("Volume Up");
            }
        }

       } else {
    // ------------------------------------------------
    // MODE HORLOGE : rendu graphique cadencé à FRAME_MS
    // ------------------------------------------------
       uint32_t now = millis();
       if (now - lastFrameClock >= FRAME_MS) {
           lastFrameClock = now;

           struct timeval tv;
           gettimeofday(&tv, nullptr);
           struct tm t;
           localtime_r(&tv.tv_sec, &t);

           float ms = tv.tv_usec / 1000.0f;
           float secF = SMOOTH_SECONDS ? (t.tm_sec + ms / 1000.0f) : (float)t.tm_sec;

           float secAngle  = secF * 6.0f;
           float minAngle  = (t.tm_min + t.tm_sec / 60.0f) * 6.0f;
           float hourAngle = ((t.tm_hour % 12) + t.tm_min / 60.0f) * 30.0f;

           for (int b = 0; b < NBANDS; b++) {
                yOff = b * BAND_H;
                drawBand(t.tm_sec, secAngle, minAngle, hourAngle);
                spr.pushSprite(FACE_X, FACE_Y + yOff);   // <-- centré horizontalement (offset 80px)
               }
          }
      }
      
}


//
