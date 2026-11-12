// ---------------------------------------------------------------------------
//  Living-frame rung 2 -- 64-color photo display firmware
//
//  Classic ESP32 (ESP32-WROOM-32 devkit) driving the rung-1 Blue Marble JPEG
//  full-screen at 320x240 in 64 colours (2 bits per channel) on a VGA monitor.
//
//  Library: FabGL 1.0.9 (pinned in platformio.ini), class fabgl::VGAController
//  -- in FabGL 1.0.9 there is no "VGA64Controller": the 64-colour bitmapped
//  controller *is* VGAController, and its 8-argument begin() takes the two
//  colour pins of each channel plus the two sync pins, so the agreed pin map
//  below is a supported configuration, not a patch of the library.
//
//  Decoder: TJpgDec R0.03 (elm-chan), vendored in ../photo-display/lib/TJpgDec.
//  Render:  src/photo_render64.h -- shared verbatim with tools/render_check.cpp
//           so the host check exercises the same decode/quantise/dither code.
//
//  AGREED PIN MAP (Stephen + Juno, 2026-11-12; Stephen has wired to it).
//  FabGL defaults with two strapping pins moved: Blue MSB 5 -> 27, VSYNC 15 -> 33.
//    Red   MSB (heavy, ~270R) GPIO22      Red   LSB (weak, ~470R) GPIO21
//    Green MSB (heavy, ~270R) GPIO19      Green LSB (weak, ~470R) GPIO18
//    Blue  MSB (heavy, ~270R) GPIO27      Blue  LSB (weak, ~470R) GPIO4
//    HSync (68R)              GPIO23      VSync (68R)             GPIO33
//  VGAController::begin() takes (red1, red0, green1, green0, blue1, blue0,
//  hsync, vsync) -- "1" is the heavy/MSB bit, so the order is 22,21,19,18,27,4,23,33.
//
//  WHY 320x240: one byte per pixel at 64 colours, so the frame buffer is
//  320*240 = 76,800 bytes of DMA-capable internal RAM -- the largest standard
//  mode on this controller that comfortably leaves room for the decoder and
//  stack (640x480 would need 307,200 bytes, more than the classic ESP32 has).
//  640x480 source / 2 = 320x240 exactly, so TJpgDec's 1/2 descale fills the
//  screen with the whole frame: no crop, no letterbox, no scaling artefacts.
//
//  The photo is embedded in flash (board_build.embed_files), decoded MCU block
//  by MCU block straight into the frame buffer: no filesystem, no SD, no WiFi,
//  and never a full RGB image buffer in RAM.
// ---------------------------------------------------------------------------

#include <fabgl.h>
#include "photo_render64.h"

using namespace fabgl;

// --- agreed pin map ---------------------------------------------------------
static const gpio_num_t redMSB   = GPIO_NUM_22;   // ~270R path -> VGA pin 1 (heavy bit)
static const gpio_num_t redLSB   = GPIO_NUM_21;   // ~470R path -> VGA pin 1 (weak bit)
static const gpio_num_t greenMSB = GPIO_NUM_19;   // ~270R path -> VGA pin 2
static const gpio_num_t greenLSB = GPIO_NUM_18;   // ~470R path -> VGA pin 2
static const gpio_num_t blueMSB  = GPIO_NUM_27;   // ~270R path -> VGA pin 3
static const gpio_num_t blueLSB  = GPIO_NUM_4;    // ~470R path -> VGA pin 3
static const gpio_num_t hsyncPin = GPIO_NUM_23;   // 68R path  -> VGA pin 13
static const gpio_num_t vsyncPin = GPIO_NUM_33;   // 68R path  -> VGA pin 14

static const int H_RES = 320;
static const int V_RES = 240;

VGAController vga;

// 64-entry raw-pixel LUT: index = (B<<4)|(G<<2)|R with each field 0..3.
// Built once through the controller's own createRawPixel(), so the bit order
// and the sync bits are the library's, not ours.
static uint8_t rawLUT[64];

// Palette census: how many of the 64 entries the frame actually uses.
static uint32_t paletteUse[64];

extern "C" {
extern const uint8_t photo_jpg_start[] asm("_binary_data_photo_jpg_start");
extern const uint8_t photo_jpg_end[]   asm("_binary_data_photo_jpg_end");
}

static void vgaPutPixel64(void *ctx, int x, int y,
                          int qr, int qg, int qb, const uint8_t *src)
{
    (void)ctx; (void)src;
    const int idx = (qb << 4) | (qg << 2) | qr;
    paletteUse[idx]++;
    vga.setRawPixel(x, y, rawLUT[idx]);
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("living-frame rung 2 -- 64-color photo display");
    Serial.printf("pins: R=%d/%d G=%d/%d B=%d/%d HSYNC=%d VSYNC=%d (msb/lsb)\n",
                  (int)redMSB, (int)redLSB, (int)greenMSB, (int)greenLSB,
                  (int)blueMSB, (int)blueLSB, (int)hsyncPin, (int)vsyncPin);
    Serial.printf("FabGL %s (pinned), mode 320x240@60Hz, 64 colors (2 bits/channel)\n",
                  FABGL_PINNED_VERSION);

    vga.begin(redMSB, redLSB, greenMSB, greenLSB, blueMSB, blueLSB,
              hsyncPin, vsyncPin);
    vga.setResolution(QVGA_320x240_60Hz);

    for (int i = 0; i < 64; i++)
        rawLUT[i] = vga.createRawPixel(RGB222(i & 3, (i >> 2) & 3, (i >> 4) & 3));
    memset(paletteUse, 0, sizeof(paletteUse));

    photo64Init();
    float L[4];
    photo64Ladder(L);
    Serial.printf("dac levels (fraction of full scale): %.3f %.3f %.3f %.3f\n",
                  L[0], L[1], L[2], L[3]);

    const size_t jpegBytes = (size_t)(photo_jpg_end - photo_jpg_start);
    Serial.printf("photo.jpg embedded: %u bytes\n", (unsigned)jpegBytes);

    static uint8_t pool[TJPGD_WORKSPACE_SIZE] __attribute__((aligned(4)));

    RenderTarget64 target;
    memset(&target, 0, sizeof(target));
    target.stream.p      = photo_jpg_start;
    target.stream.left   = jpegBytes;
    target.put           = vgaPutPixel64;
    target.ctx           = &vga;
    target.cfg.screenW   = H_RES;
    target.cfg.screenH   = V_RES;
    target.cfg.scaleShift = 1;      // 1/2 descale: 640x480 -> 320x240

    JDEC jd;
    memset(&jd, 0, sizeof(jd));

    unsigned long t0 = millis();
    JRESULT res = jd_prepare(&jd, photo64InFunc, pool, sizeof(pool), &target);
    if (res != JDR_OK) {
        Serial.printf("ERROR: jd_prepare failed: %s (%d)\n", jdErrorString(res), (int)res);
        while (true) delay(1000);
    }
    Serial.printf("jpeg: %ux%u, %u component(s) -> %ux%u screen (1/2 descale)\n",
                  (unsigned)jd.width, (unsigned)jd.height, (unsigned)jd.ncomp,
                  (unsigned)H_RES, (unsigned)V_RES);

    res = jd_decomp(&jd, photo64OutFunc, target.cfg.scaleShift);
    if (res != JDR_OK) {
        Serial.printf("ERROR: jd_decomp failed: %s (%d)\n", jdErrorString(res), (int)res);
        while (true) delay(1000);
    }

    int used = 0;
    uint32_t minUse = 0xFFFFFFFFu, maxUse = 0;
    for (int i = 0; i < 64; i++) {
        if (paletteUse[i]) used++;
        if (paletteUse[i] < minUse) minUse = paletteUse[i];
        if (paletteUse[i] > maxUse) maxUse = paletteUse[i];
    }
    Serial.printf("frame drawn in %lu ms (%u pixels), free heap: %u bytes\n",
                  millis() - t0, (unsigned)target.pixelsDrawn,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    Serial.printf("palette: %d of 64 colors used (counts %u..%u)\n",
                  used, (unsigned)minUse, (unsigned)maxUse);
    Serial.println("holding image");
}

void loop()
{
    // Static frame held in the frame buffer; the I2S/DMA interrupt keeps
    // scanning it out. Nothing is redrawn, so the picture is stable.
    delay(1000);
}
