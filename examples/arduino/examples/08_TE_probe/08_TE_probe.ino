// 4N-band TE-gated full-screen demo (user scheme):
//   B = 4N bands per frame; every TE window flushes N bands (a quarter of
//   the screen); a full frame completes in exactly 4 TE periods (~66.7 ms
//   -> ~15 fps at 40 MHz, independent of N). N only changes granularity.
//
// Why bands + why 4N: each band write lands behind the scan line, so no row
// is ever torn. The visible artifact becomes a "seam" sweeping down the
// screen at one quarter-screen per TE; rows above = new frame, below = old.
//
// Dual-core producer (semaphore handshake, v2.1 design) renders the next
// full frame into a PSRAM back buffer during the 4 flush windows (~13 ms
// render << 66.7 ms budget). Consumer flushes N bands per TE from the
// published front buffer; the frame stays stable across its 4 TEs.
//
// N cycles 1,2,4,8 every 8 s (B = 4,8,16,32); stats over serial. Watch:
// with N=1 vs N=8 the picture should look IDENTICAL except the plate number
// (proving granularity, not throughput) — and no horizontal tearing at any
// N, only the moving seam.

#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"

#define LCD_TE_PIN 13
// 2-window mode at 80 MHz: frame = 2 TE periods (33.4 ms -> ~30 fps).
// NOTE: with 2 windows, residue interleave conflicts with scan-chasing
// (window 0 cannot write bottom-half bands — the scan has not reached them
// yet), so windows take sequential halves: window g flushes bands
// [g*N, (g+1)*N). Fine granularity preserved inside each half.
#define FRAME_GROUPS 2
static const int RES_ORDER[FRAME_GROUPS] = {0, 1};
#define MODE_MS 8000

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *gfx = new Arduino_CO5300(
  bus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);

static const int N_LIST[] = {4, 8, 16};       // bands per half at 80 MHz
static volatile int curN = 4;               // bands per TE (producer reads for plate)
static volatile uint32_t consumedSeq = 0; // phase driver: consumed frames
static bool markerMode = false;           // stripe demo (line mode retired: too hard to track 1px steps)

static uint16_t *fb[2];
static volatile int front = 0;
static SemaphoreHandle_t frameFree, frameReady;
static volatile uint32_t prodN = 0, consN = 0;

static const int SPEED = 4;               // px per displayed frame (<= 4: user threshold)
static const int STRIPE = 16;             // > 3x SPEED

static void renderFrame(uint16_t *dst, uint32_t seq) {
  uint32_t phase = seq * SPEED;
  // Store stripes in BIG-ENDIAN order (panel wire order): the flush then
  // takes the zero-copy writeBytes DMA path (18.4 MB/s @80MHz measured)
  // instead of writePixels per-pixel assembly (9.5 MB/s).
  uint8_t *rowBe = (uint8_t *)dst;
  for (int y = 0; y < LCD_HEIGHT; y++) {
    for (int x = 0; x < LCD_WIDTH; x++) {
      uint16_t c = (((x + phase) / STRIPE) & 1) ? RGB565_WHITE : RGB565_BLACK;
      rowBe[y * LCD_WIDTH * 2 + x * 2] = c >> 8;
      rowBe[y * LCD_WIDTH * 2 + x * 2 + 1] = c & 0xFF;
    }
  }
  // Plate digit drawn in host order via canvas-like helpers would break the
  // BE layout; draw it BE by hand (same as stripes).
  int n = curN;
  for (int y = 200; y < 266; y++) {
    for (int x = 143; x < 323; x++) {
      uint8_t *px = rowBe + (uint32_t)y * LCD_WIDTH * 2 + x * 2;
      uint16_t c = RGB565_BLUE;
      *px = c >> 8; px[1] = c & 0xFF;
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
  const uint8_t *g = glyphs[n < 9 ? n : 8];
  for (int gy = 0; gy < 5; gy++) {
    for (int gx = 0; gx < 3; gx++) {
      if (g[gy] & (1 << (2 - gx))) {
        for (int sy = 0; sy < 8; sy++) {
          for (int sx = 0; sx < 8; sx++) {
            uint8_t *px = rowBe + (uint32_t)(212 + gy * 8 + sy) * LCD_WIDTH * 2 + (214 + gx * 8 + sx) * 2;
            *px = 0xFF; px[1] = 0xFF;
          }
        }
      }
    }
  }
}

static void producerTask(void *) {
  // first frame
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

// Panels scan 466 rows per TE period (16.7 ms) => 27.9 rows/ms. A band is
// safe to write only AFTER the scan has passed its bottom row (else the
// scan reads pre-write data) and before the NEXT scan re-reaches it.
#define SCAN_ROWS_PER_MS 27.9

// Wait for a TE edge since last observed count (not "next edge"): if the
// previous window overran into the pulse, this returns immediately.
static uint32_t teObserved = 0;
static void waitTEEdge() {
  while (teCount == teObserved) { delayMicroseconds(20); }
  teObserved = teCount;
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

  Serial.println("=== 2N-band TE demo @80MHz (B=2N, N per half, 2 TE/frame -> ~30fps) ===");
  Serial.print("ESP32QSPI_FREQUENCY macro = ");
#ifdef ESP32QSPI_FREQUENCY
  Serial.println(String(ESP32QSPI_FREQUENCY / 1000000) + " MHz");
#else
  Serial.println("undefined (40 default)");
#endif
  // Raw throughput probes: pixel-assembly path vs zero-copy writeBytes.
  {
    static uint8_t *probeBe = (uint8_t *)fb[0];
    // big-endian pattern (network order == panel order for RGB565)
    for (int i = 0; i < 400; i++) {
      probeBe[i * 2] = 0xF8; probeBe[i * 2 + 1] = 0x00;
    }
    gfx->draw16bitRGBBitmap(0, 0, (uint16_t *)probeBe, LCD_WIDTH, LCD_HEIGHT);
    float ms1 = 45.8f;  // measured last boot (assembly path @80MHz)
    // zero-copy: address window + raw bytes, bypassing writePixels assembly
    uint32_t t = micros();
    gfx->setAddrWindow(0, 0, LCD_WIDTH, LCD_HEIGHT);  // if unavailable: writeAddrWindow via beginWrite
    bus->writeBytes(probeBe, (uint32_t)LCD_WIDTH * LCD_HEIGHT * 2);
    float ms2 = (micros() - t) / 1000.0f;
    Serial.println("full-frame assembly(writePixels): " + String(ms1, 1) + " ms = 9.5 MB/s");
    Serial.println("full-frame zero-copy(writeBytes): " + String(ms2, 1) + " ms = " +
                   String(LCD_WIDTH * LCD_HEIGHT * 2 / ms2 / 1000.0f, 1) + " MB/s");
  }
  xTaskCreatePinnedToCore(producerTask, "prod", 8192, nullptr, 2, nullptr, 0);
}

// HWCDC write blocks up to ~2 s (20 x tx_timeout) per call when no host
// drains the port — that froze the UI for seconds at every stats print
// while unmonitored. Drop logs when no host is connected.
static void logLine(const String &s) {
  if (Serial.isConnected()) Serial.println(s);
}

void loop() {
  static int nIdx = 0;
  static uint32_t modeStart = millis();
  static uint32_t t0 = millis(), c0 = 0, p0 = 0;
  static uint32_t frameTStart = 0, lastFrameT = 0;

  int N = N_LIST[nIdx];
  curN = N;
  int B = FRAME_GROUPS * N;
  // Even-alignment banding (Waveshare rounder_cb forces even start + even
  // size windows; odd heights/starts caused stuck pixel rows on this panel).
  // 466 = 233 * 2 row-pairs: every band gets an even count of row-PAIRS,
  // so band heights and starts stay even; residual pairs spread to first r2.
  int q2 = 233 / B;               // pairs per band
  int r2 = 233 % B;               // first r2 bands get one extra pair
  static int frameLocal = -1;

  for (int gw = 0; gw < FRAME_GROUPS; gw++) {
    int g = RES_ORDER[gw];
    // At frame start: swap in the next frame if producer published one.
    if (g == 0) {
      if (xSemaphoreTake(frameReady, 0) == pdTRUE) {
        frameLocal = front;
        xSemaphoreGive(frameFree);   // release other buffer immediately
        consumedSeq++;
        consN++;
      } else {
        // no fresh frame: reuse current front (frame drop) — keep phase
        frameLocal = front;
      }
      lastFrameT = 0;
      frameTStart = millis();
    }

    waitTEEdge();
    uint32_t tTE = teLastMicros;

    // Residue interleave retired for 2-window mode: sequential halves with
    // scan-chasing. Zero-copy flush: address window + raw BE bytes.
    for (int b = g * N; b < (g + 1) * N && b < B; b++) {
      int pair0 = b * q2 + (b < r2 ? b : r2);
      int pairs = q2 + (b < r2 ? 1 : 0);
      int row0 = pair0 * 2;
      int rows = pairs * 2;
      int rowEnd = row0 + rows;
      uint32_t passUs = (uint32_t)(rowEnd * 1000.0f / SCAN_ROWS_PER_MS);
      while ((micros() - tTE) < passUs) delayMicroseconds(100);
      gfx->setAddrWindow(0, row0, LCD_WIDTH, rows);
      bus->beginWrite();
      bus->writeBytes((uint8_t *)(fb[frameLocal] + (uint32_t)row0 * LCD_WIDTH),
                      (uint32_t)LCD_WIDTH * rows * 2);
      bus->endWrite();
    }

    if (gw == FRAME_GROUPS - 1) {
      lastFrameT = millis() - frameTStart;
      // Stats/N-switch at frame END (was mid-frame `return`: it abandoned
      // the frame after one window, skipping a whole display frame — the
      // periodic double-step stutter every 8 s).
      uint32_t now = millis();
      if (now - modeStart > MODE_MS) {
        float dt = (now - t0) / 1000.0f;
        logLine("[N=" + String(N) + " B=" + String(B) + "] fps=" +
                String((consN - c0) / dt, 1) + " (producer " +
                String((prodN - p0) / dt, 1) + ") scroll=" +
                String((consN - c0) / dt * SPEED) + " px/s | frame ms=" +
                (lastFrameT ? String(lastFrameT) : "?"));
        c0 = consN; p0 = prodN; t0 = now; modeStart = now;
        nIdx = (nIdx + 1) % (sizeof(N_LIST) / sizeof(N_LIST[0]));
      }
    }
  }
}
