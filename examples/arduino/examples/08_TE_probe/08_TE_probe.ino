// Three-mode TE-sync comparison @80 MHz zero-copy (user experiment):
//   Mode 1 INTERLACE : 60 fields/s, each field writes every other row-PAIR
//                      (rows 0-1,4-5,8-9... then 2-3,6-7...) chasing the
//                      scan; full frame = 2 fields (~30 fps). Seam = row-pair
//                      comb (unwritten pairs show previous field).
//   Mode 2 4WIN-RES  : 4 windows, residue classes 0,2,1,3, N=8 bands each,
//                      scan-chased (~15 fps). Seam = finest uniform grain
//                      (the 40 MHz final-look, now with bandwidth slack).
//   Mode 3 2WIN-HALF : 2 sequential half-screen windows, N=8 (~30 fps).
//                      Seam = half-screen alternation (previous final).
// Blue plate shows the mode number; cycles every 8 s.

#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"

#define LCD_TE_PIN 13
#define SCAN_ROWS_PER_MS 27.9   // 466 rows / 16.7 ms
#define MODE_MS 8000
// TE2-start offset for flip modes: absorbs TE pulse width (Tvdh ~1 ms),
// ISR latency, and TE-vs-scan-line-0 phase (SETTSL default). If a tear sits
// at the TOP, increase; at the BOTTOM, decrease.
static uint32_t flipDelayUs = 500;   // calibrated: TE leads row-0 scan by
                                    // >=0.5 ms (safe at all tested offsets;
                                    // 500 us chosen to absorb ISR jitter)

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

static const int MODE_LIST[] = {7};          // rect-zoo scheduler capstone
static volatile int curMode = 7;
// Zoo rects [x0,y0,w,h] shared with producer (x0,w even for CASET).
static volatile int zooG[3][4] = {{123,80,80,60},{83,300,300,30},{333,60,50,300}};
static volatile int curY0 = 0;
static volatile int curH = 466;
static volatile int curAnchor = 1;         // 1 center, 2 top, 3 bottom
static uint32_t maxDmaUs = 0;
static int lastTes = 0;                   // TEs consumed by last frame drain
static bool usedFallback = false;         // last frame fell back to full flip          // producer reads for plate digit
static volatile uint32_t consumedSeq = 0; // phase driver: consumed frames

static uint16_t *fb[2];
static volatile int front = 0;
static SemaphoreHandle_t frameFree, frameReady;
static volatile uint32_t prodN = 0, consN = 0;

static const int SPEED = 4;               // px per displayed frame (user threshold)
static const int STRIPE = 16;             // > 3x SPEED

static void renderFrame(uint16_t *dst, uint32_t seq) {
  uint32_t phase = seq * SPEED;
  // BIG-ENDIAN in-memory layout == panel wire order -> zero-copy writeBytes.
  uint8_t *rowBe = (uint8_t *)dst;
  int py = 200;   // plate y: modes 4-6 center it in band; zoo pins bottom
  // Mode-7: black field + zoo rects (scrolling stripes + 2px red inner
  // border). A torn flush breaks the border lines — visual tear oracle.
  if (curMode == 7) {
    py = 366;
    memset(rowBe, 0, LCD_WIDTH * LCD_HEIGHT * 2);
    for (int zi = 0; zi < 3; zi++) {
      int x0 = zooG[zi][0], y0 = zooG[zi][1], w = zooG[zi][2], h = zooG[zi][3];
      for (int y = y0; y < y0 + h; y++) {
        uint8_t *r = rowBe + (uint32_t)y * LCD_WIDTH * 2;
        for (int x = x0; x < x0 + w; x++) {
          uint16_t c;
          bool edge = (x < x0 + 2) || (x >= x0 + w - 2) || (y < y0 + 2) || (y >= y0 + h - 2);
          if (edge) c = RGB565_RED;
          else c = (((x + phase) / STRIPE) & 1) ? RGB565_WHITE : RGB565_BLACK;
          r[x * 2] = c >> 8; r[x * 2 + 1] = c & 0xFF;
        }
      }
    }
  } else {
  int bandTop = 0, bandBot = LCD_HEIGHT;
  if (curMode == 6) {
    bandTop = curY0; bandBot = curY0 + curH;
    if (curH >= 100) py = ((bandTop + bandBot - 66) / 2) & ~1;
  }
  for (int y = bandTop; y < bandBot; y++) {
    for (int x = 0; x < LCD_WIDTH; x++) {
      uint16_t c = (((x + phase) / STRIPE) & 1) ? RGB565_WHITE : RGB565_BLACK;
      rowBe[y * LCD_WIDTH * 2 + x * 2] = c >> 8;
      rowBe[y * LCD_WIDTH * 2 + x * 2 + 1] = c & 0xFF;
    }
  }
  }
  // Blue plate + digit (BE).
  for (int y = py; y < py + 66; y++) {
    for (int x = 143; x < 323; x++) {
      uint8_t *px = rowBe + (uint32_t)y * LCD_WIDTH * 2 + x * 2;
      *px = 0x1F; px[1] = 0x3F;   // RGB565_BLUE in BE
    }
  }
  static const uint8_t glyphs[9][5] = {
    {},
    {0b010,0b110,0b010,0b010,0b111},
    {0b110,0b001,0b010,0b100,0b111},
    {0b110,0b001,0b010,0b001,0b110},
    {0b101,0b101,0b111,0b001,0b001},
    {0b111,0b100,0b110,0b001,0b110},
    {0b011,0b100,0b111,0b101,0b111},
    {0b111,0b001,0b010,0b010,0b010},
    {0b111,0b101,0b111,0b101,0b111},
  };
  // Plate digit: anchor number for mode 6, mode number otherwise.
  // Digit baseline follows plate y (py) so it stays inside the band.
  const uint8_t *g = glyphs[curMode == 6 ? curAnchor : curMode];
  int dy = py + 12;
  for (int gy = 0; gy < 5; gy++) {
    for (int gx = 0; gx < 3; gx++) {
      if (g[gy] & (1 << (2 - gx))) {
        for (int sy = 0; sy < 8; sy++) {
          for (int sx = 0; sx < 8; sx++) {
            uint8_t *px = rowBe + (uint32_t)(dy + gy * 8 + sy) * LCD_WIDTH * 2 + (214 + gx * 8 + sx) * 2;
            *px = 0xFF; px[1] = 0xFF;
          }
        }
      }
    }
  }
}

static void producerTask(void *) {
  renderFrame(fb[0], 0);
  xSemaphoreGive(frameReady);
  for (;;) {
    if (xSemaphoreTake(frameFree, portMAX_DELAY) != pdTRUE) continue;
    int back = 1 - front;
    renderFrame(fb[back], consumedSeq + 1);
    noInterrupts(); front = back; interrupts();
    prodN++;
    xSemaphoreGive(frameReady);
  }
}

volatile uint32_t teCount = 0;
volatile uint32_t teLastMicros = 0;
void IRAM_ATTR onTE() { teCount++; teLastMicros = micros(); }

static uint32_t teObserved = 0;
static void waitTEEdge() {
  while (teCount == teObserved) { delayMicroseconds(20); }
  teObserved = teCount;
}

// Chase the scan: hold until the scan (27.9 rows/ms from TE) passed rowEnd.
static void chaseScan(uint32_t tTE, int rowEnd) {
  uint32_t passUs = (uint32_t)(rowEnd * 1000.0f / SCAN_ROWS_PER_MS);
  while ((micros() - tTE) < passUs) delayMicroseconds(50);
}

static void flushBand(int frameLocal, int row0, int rows) {
  gfx->setAddrWindow(0, row0, LCD_WIDTH, rows);
  bus->beginWrite();
  bus->writeBytes((uint8_t *)(fb[frameLocal] + (uint32_t)row0 * LCD_WIDTH),
                  (uint32_t)LCD_WIDTH * rows * 2);
  bus->endWrite();
}

static int takeNewFrame() {   // returns buffer index to display
  static int frameLocal = -1;
  if (xSemaphoreTake(frameReady, 0) == pdTRUE) {
    frameLocal = front;
    xSemaphoreGive(frameFree);
    consumedSeq++;
    consN++;
  } else {
    frameLocal = front;   // frame drop: keep phase
  }
  return frameLocal;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(LCD_TE_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(LCD_TE_PIN), onTE, RISING);

  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(180);

  fb[0] = (uint16_t *)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
  fb[1] = (uint16_t *)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
  memset(fb[0], 0, LCD_WIDTH * LCD_HEIGHT * 2);
  memset(fb[1], 0, LCD_WIDTH * LCD_HEIGHT * 2);
  frameFree = xSemaphoreCreateBinary();
  frameReady = xSemaphoreCreateBinary();

  Serial.println("=== 4-mode TE @80MHz: 4=fullframe-flip 1=interlace 2=4win-res 3=2win-half ===");
  xTaskCreatePinnedToCore(producerTask, "prod", 8192, nullptr, 2, nullptr, 0);
}

// HWCDC write blocks up to ~2 s per call with no host draining the port.
static void logLine(const String &s) {
  if (Serial.isConnected()) Serial.println(s);
}

void loop() {
  static int mIdx = 0;
  static uint32_t modeStart = millis();
  static uint32_t t0 = millis(), c0 = 0, p0 = 0;

  int mode = MODE_LIST[mIdx];
  curMode = mode;
  uint32_t fstart = millis();

  if (mode == 4) {
    // FULL-FRAME PAGE-FLIP (user proposal + timing refinement): one DMA of
    // the whole framebuffer per 2 TE. Must start at the SECOND TE (t=16.7ms):
    // write front (19.7 rows/ms) then stays ahead of scan pass 2 for its
    // whole pass and finishes before pass 3 catches up (done by 40.3ms <
    // 50.1ms) -> every scan pass reads a complete frame, zero mixed states.
    // Starting at TE1 instead would put a fixed tear seam at row ~330.
    int fl = takeNewFrame();
    waitTEEdge();
    waitTEEdge();   // TE2: pass 2 starts reading old frame from GRAM — GO
    while ((uint32_t)(micros() - teLastMicros) < flipDelayUs) delayMicroseconds(50);
    gfx->setAddrWindow(0, 0, LCD_WIDTH, LCD_HEIGHT);
    bus->beginWrite();
    bus->writeBytes((uint8_t *)fb[fl], (uint32_t)LCD_WIDTH * LCD_HEIGHT * 2);
    bus->endWrite();
    // DMA ends ~40.3ms; passes 3-4 display the new frame.
  } else if (mode == 5) {
    // ADAPTIVE PARTIAL FLIP with boundary-marker: each band edge gets a 1px
    // marker line (red) — if a flush ever mixes frames, the marker breaks.
    static const int H_LIST[] = {60, 120, 240, 360, 466};
    static int hIdx = 0;
    static uint32_t hStart = millis();
    static int prevH = -1;
    int dirtyH = H_LIST[hIdx];
    curH = dirtyH;
    int y0 = (LCD_HEIGHT - dirtyH) / 2 & ~1;
    if (dirtyH >= LCD_HEIGHT) y0 = 0;
    int fl = takeNewFrame();
    // Boundary-change guard: when the band height changes, the OLD band's
    // edge rows are still in GRAM. Flush the union (old ∪ new) once so no
    // stale edge survives — the "slight tearing" seen in modes 4/5 at
    // switch moments was stale band edges, not mid-DMA tearing.
    int yu0 = 0, yu1 = LCD_HEIGHT;
    if (dirtyH < LCD_HEIGHT) {
      if (prevH >= 0 && prevH != dirtyH) {
        int p0 = (LCD_HEIGHT - prevH) / 2 & ~1;
        if (prevH >= LCD_HEIGHT) p0 = 0;
        yu0 = min(p0, y0); yu1 = max(p0 + prevH, y0 + dirtyH);
      } else {
        yu0 = y0; yu1 = y0 + dirtyH;
      }
    }
    prevH = dirtyH;
    waitTEEdge();
    waitTEEdge();
    while ((uint32_t)(micros() - teLastMicros) < flipDelayUs) delayMicroseconds(50);
    uint32_t t2 = micros();
    gfx->setAddrWindow(0, yu0, LCD_WIDTH, yu1 - yu0);
    bus->beginWrite();
    bus->writeBytes((uint8_t *)fb[fl] + (uint32_t)yu0 * LCD_WIDTH * 2,
                    (uint32_t)LCD_WIDTH * (yu1 - yu0) * 2);
    bus->endWrite();
    uint32_t dmaUs = micros() - t2;
    if (dmaUs > maxDmaUs) maxDmaUs = dmaUs;
    if (millis() - hStart > 5000) {
      logLine("[flip h=" + String(dirtyH) + " union=" + String(yu1 - yu0) + "] dmaUs max=" + String(maxDmaUs) + " (theory " + String((yu1 - yu0) * 19700 / 466) + ")");
      maxDmaUs = 0;
      hIdx = (hIdx + 1) % 5;
      hStart = millis();
    }
  } else if (mode == 6) {
    // GROWTH DEMO (user spec): stripes scroll continuously; a band grows
    // 60→466→60 rows (ramped over ~4 s per half-cycle), anchored at center /
    // top / bottom in turn. Correct engine must handle any (y0,h) each frame
    // with zero tearing. Union-flush on band change absorbs stale edges.
    static int anchor = 1;
    static int h = 60;
    static int hDir = 4;
    static uint32_t anchorStart = millis();
    static int prevY0 = -1, prevH = -1;
    h += hDir;
    if (h >= 466) { h = 466; hDir = -hDir; }
    if (h < 60)   { h = 60;   hDir = -hDir; }
    if (millis() - anchorStart > 12000) {
      anchor = anchor % 3 + 1;             // 1 center -> 2 top -> 3 bottom
      anchorStart = millis();
    }
    curAnchor = anchor;
    curH = h;
    int y0;
    if (anchor == 2)      y0 = 0;
    else if (anchor == 3) y0 = LCD_HEIGHT - h;
    else                  y0 = (LCD_HEIGHT - h) / 2;
    y0 &= ~1;  h &= ~1;
    curY0 = y0;
    int fl = takeNewFrame();
    int yu0 = y0, yu1 = y0 + h;
    if (prevY0 >= 0 && prevH != h) {        // union with previous band
      yu0 = min(yu0, prevY0);
      yu1 = max(yu1, prevY0 + prevH);
    }
    prevY0 = y0; prevH = h;
    waitTEEdge();
    waitTEEdge();
    while ((uint32_t)(micros() - teLastMicros) < flipDelayUs) delayMicroseconds(50);
    gfx->setAddrWindow(0, yu0, LCD_WIDTH, yu1 - yu0);
    bus->beginWrite();
    bus->writeBytes((uint8_t *)fb[fl] + (uint32_t)yu0 * LCD_WIDTH * 2,
                    (uint32_t)LCD_WIDTH * (yu1 - yu0) * 2);
    bus->endWrite();
  } else if (mode == 7) {
    // RECT-ZOO SCHEDULER (capstone): per-TE admission by the corridor test
    // in the (t,row) plane. A DMA for rows [y,y+ch) width w is safe iff
    //   τ ≥ MARGIN + y/S   and   τ + ch·1000/W ≤ MARGIN + P + (y+ch)/S
    // (W = 19.7·w/466 rows/ms, S = 27.9, P = 16950 µs, MARGIN = 500 µs).
    // Tall-thin rects exceed the corridor -> split into chunks of
    // ch ≤ (P−MARGIN)/(1000/W − 1000/S), one+ per TE.
    const float P_US = 16950.0f, MARGIN = 500.0f;
    static int dir0 = 3, dir1 = 5;
    // animate: block bounces vertically, bar bounces horizontally
    zooG[0][1] += dir0; if (zooG[0][1] < 30 || zooG[0][1] > 160) { dir0 = -dir0; zooG[0][1] += 2 * dir0; }
    zooG[1][0] += dir1; if (zooG[1][0] < 20 || zooG[1][0] > 120) { dir1 = -dir1; zooG[1][0] += 2 * dir1; }
    int fl = takeNewFrame();
    // ---- PLAN (pure simulation, no bus traffic) ----
    // Walk the rects in order, simulate chunked transfers against the scan
    // line: chunk k of rect i is admitted at the earliest TE where corridor
    // holds (τ ≥ MARGIN + y/S  and  τ + ch·1000/W ≤ MARGIN + P + (y+ch)/S).
    // Feasible ⟺ every chunk lands in some window AND all work drains within
    // one frame (2 TE for flip semantics; use 3 TE cap as frame budget).
    // Infeasible -> FALL BACK to full-frame flip (mode-4 engine).
    struct Plan { int zi, y, ch; uint32_t tau; int teIdx; };
    Plan plan[24];
    int nPlan = 0;
    float tNow = MARGIN;                     // us after current TE
    int teIdx = 0;
    bool feasible = true;
    for (int zi = 0; zi < 3 && feasible; zi++) {
      int x0 = zooG[zi][0], y0 = zooG[zi][1], w = zooG[zi][2], h = zooG[zi][3];
      float W = 19.7f * w / 466.0f;
      float invDiff = 1000.0f / W - 1000.0f / 27.9f;
      int chMax = (int)((P_US - MARGIN) / invDiff) & ~1;
      if (chMax < 2) chMax = 2;
      int y = y0;
      while (y < y0 + h && feasible) {
        int ch = min(chMax, y0 + h - y);
        float dur = ch * 1000.0f / W;
        // earliest tau at this TE satisfying corridor
        float tauMin = MARGIN + y * 1000.0f / 27.9f;
        float tau = max(tNow, tauMin);
        float deadline = MARGIN + P_US + (y + ch) * 1000.0f / 27.9f;
        if (tau + dur <= deadline) {
          // fits in current TE window
        } else {
          // defer to next TE
          teIdx++;
          if (teIdx > 2) { feasible = false; break; }   // frame budget
          tNow = MARGIN;
          tau = max(MARGIN, tauMin);
          deadline = MARGIN + P_US + (y + ch) * 1000.0f / 27.9f;
          if (tau + dur > deadline) { feasible = false; break; }
        }
        if (nPlan < 24) { plan[nPlan] = {zi, y, ch, (uint32_t)tau, teIdx}; nPlan++; }
        tNow = tau + dur;
        y += ch;
      }
    }
    lastTes = teIdx + 1;
    if (!feasible) {
      // FALLBACK: full-frame flip (mode-4 engine)
      waitTEEdge(); waitTEEdge();
      uint32_t t0us = micros();
      while ((uint32_t)(micros() - teLastMicros) < flipDelayUs) delayMicroseconds(50);
      gfx->setAddrWindow(0, 0, LCD_WIDTH, LCD_HEIGHT);
      bus->beginWrite();
      bus->writeBytes((uint8_t *)fb[fl], (uint32_t)LCD_WIDTH * LCD_HEIGHT * 2);
      bus->endWrite();
      usedFallback = true;
    } else {
      // EXECUTE plan: TE alignment by teIdx; wait real TE edges as needed
      int lastTe = 0;
      for (int k = 0; k < nPlan; k++) {
        while (lastTe < plan[k].teIdx) { waitTEEdge(); lastTe++; }
        while ((uint32_t)(micros() - teLastMicros) < plan[k].tau) delayMicroseconds(50);
        int x0 = zooG[plan[k].zi][0], w = zooG[plan[k].zi][2];
        gfx->setAddrWindow(x0, plan[k].y, w, plan[k].ch);
        bus->beginWrite();
        bus->writeBytes((uint8_t *)fb[fl] + (uint32_t)(plan[k].y * LCD_WIDTH + x0) * 2,
                        (uint32_t)w * plan[k].ch * 2);
        bus->endWrite();
      }
      usedFallback = false;
    }
  } else if (mode == 1) {
    // INTERLACE: frame = 2 fields; field f writes row-pairs f, f+2, f+4...
    int fl = -1;
    for (int f = 0; f < 2; f++) {
      if (f == 0) fl = takeNewFrame();
      waitTEEdge();
      uint32_t tTE = teLastMicros;
      for (int pr = f; pr < 233; pr += 2) {
        int row0 = pr * 2;
        chaseScan(tTE, row0 + 2);
        flushBand(fl, row0, 2);
      }
    }
  } else {
    // Banded modes: 4win residue-interleaved (N=8) or 2win halves (N=8).
    const int groups = (mode == 2) ? 4 : 2;
    static const int res4[4] = {0, 2, 1, 3};
    const int N = 8;
    const int B = groups * N;
    const int q2 = 233 / B;        // row-pairs per band
    const int r2 = 233 % B;        // first r2 bands get one extra pair
    int fl = -1;
    for (int gw = 0; gw < groups; gw++) {
      if (gw == 0) fl = takeNewFrame();
      waitTEEdge();
      uint32_t tTE = teLastMicros;
      if (mode == 2) {
        for (int b = res4[gw]; b < B; b += 4) {
          int pair0 = b * q2 + (b < r2 ? b : r2);
          int rows = (q2 + (b < r2 ? 1 : 0)) * 2;
          chaseScan(tTE, pair0 * 2 + rows);
          flushBand(fl, pair0 * 2, rows);
        }
      } else {
        for (int b = gw * N; b < (gw + 1) * N; b++) {
          int pair0 = b * q2 + (b < r2 ? b : r2);
          int rows = (q2 + (b < r2 ? 1 : 0)) * 2;
          chaseScan(tTE, pair0 * 2 + rows);
          flushBand(fl, pair0 * 2, rows);
        }
      }
    }
  }

  // Stats & mode switch at frame END (mid-frame switching drops a frame).
  uint32_t now = millis();
  if (now - modeStart > MODE_MS) {
    logLine("[mode " + String(mode) + "] fps=" +
            String((consN - c0) * 1000.0f / (now - t0), 1) + " | frame ms=" +
            String(now - fstart) + " | TEs=" + String(lastTes) +
            (usedFallback ? " FALLBACK" : " chunked"));
    c0 = consN; p0 = prodN; t0 = now; modeStart = now;
    mIdx = (mIdx + 1) % (sizeof(MODE_LIST) / sizeof(MODE_LIST[0]));
  }
}
