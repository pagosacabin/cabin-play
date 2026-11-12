// ---------------------------------------------------------------------------
//  Living-frame rung 2b -- 64-color diagnostic + two photos
//
//  Classic ESP32 (ESP32-WROOM-32 devkit) driving a VGA monitor at 320x240@60Hz
//  with 64 colours (2 bits per channel) through FabGL 1.0.9.
//
//  ROUND 2 (Juno, 2026-11-12, after Stephen's photo of rung 2 came back
//  "same as before, the Earth is still a dot puzzle and the black is
//  speckled"). Three changes, all firmware, no rewiring:
//
//   1. DIAGNOSTIC RAMP SCREEN, drawn at boot and held for 30 s. Per channel
//      (R, G, B) four patches in the order 0, LSB only, MSB only, both -- so a
//      photograph of the screen shows whether each channel's two bits arrive
//      in the right order, at the right weight, on the right colour. Then a
//      four-step grey ramp, then a dithered black->white gradient. The pin map
//      and the 320x240@60 mode are unchanged; this screen is how round 1's
//      "bit order or wiring fault?" question gets answered by evidence.
//
//   2. A SECOND PHOTO with saturated colour -- a rainbow lorikeet, CC BY 4.0,
//      see data/PHOTO-SOURCE.md -- through the identical pipeline, so the 64
//      colours are judged on a subject that uses them (45 of 64 palette entries
//      on the host check, against 24 for Blue Marble). The Blue Marble photo
//      from round 1 is still embedded and unchanged.
//
//   3. A GAMMA-CORRECT QUANTISER (PHOTO64_MODE_PERCEPTUAL in
//      src/photo_render64.h). Round 1 dithered to the right average *voltage*;
//      a panel fed VGA maps voltage to luminance as V^2.2, so that rendered
//      everything too bright and turned shadows into sparse mid-grey dots --
//      the speckled black. Dithering in the light domain instead drops the
//      near-black leak from 3.1% to 0.0% (Blue Marble) and from 19.3% to 1.8%
//      (macaw) on the host check, with the light-domain 8x8 block RMS falling
//      from 6.05 to 2.36 and 6.68 to 2.37. Round 1's pipeline is still
//      selectable at runtime for A/B.
//
//  SCREENS (serial, 115200 on the CP2102):
//      1  diagnostic ramp            2  lorikeet, perceptual (default)
//      3  Blue Marble, perceptual    4  lorikeet, round-1 pipeline
//      5  Blue Marble, round-1 pipeline
//  Boot order: ramp (30 s, so it can be photographed) -> lorikeet. Power-cycle to
//  see the ramp again; no terminal is needed for the default sequence.
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
//  The photos are embedded in flash (board_build.embed_files), decoded MCU
//  block by MCU block straight into the frame buffer: no filesystem, no SD, no
//  WiFi, and never a full RGB image buffer in RAM.
// ---------------------------------------------------------------------------

#include <fabgl.h>
#include "photo_render64.h"
#include "ramp_screen.h"

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

// Compile-time guard on the agreed table. If any pin is edited without the
// table (and Stephen's wiring) moving with it, the build fails instead of
// silently lighting the wrong DAC bit.
static_assert(redMSB   == GPIO_NUM_22, "red MSB must be GPIO22 (~270R, VGA pin 1)");
static_assert(redLSB   == GPIO_NUM_21, "red LSB must be GPIO21 (~470R, VGA pin 1)");
static_assert(greenMSB == GPIO_NUM_19, "green MSB must be GPIO19 (~270R, VGA pin 2)");
static_assert(greenLSB == GPIO_NUM_18, "green LSB must be GPIO18 (~470R, VGA pin 2)");
static_assert(blueMSB  == GPIO_NUM_27, "blue MSB must be GPIO27 (~270R, VGA pin 3)");
static_assert(blueLSB  == GPIO_NUM_4,  "blue LSB must be GPIO4 (~470R, VGA pin 3)");
static_assert(hsyncPin == GPIO_NUM_23, "HSync must be GPIO23 (68R, VGA pin 13)");
static_assert(vsyncPin == GPIO_NUM_33, "VSync must be GPIO33 (68R, VGA pin 14)");

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
extern const uint8_t parrot_jpg_start[] asm("_binary_data_parrot_jpg_start");
extern const uint8_t parrot_jpg_end[]   asm("_binary_data_parrot_jpg_end");
}

struct Picture {
    const uint8_t *start;
    const uint8_t *end;
    const char    *name;
};

static const Picture PIC_BLUE   = { photo_jpg_start,  photo_jpg_end,  "blue-marble" };
static const Picture PIC_PARROT = { parrot_jpg_start, parrot_jpg_end, "lorikeet" };

// --- drawing helpers --------------------------------------------------------

// The ramp screen's drawing code lives in src/ramp_screen.h so that the host
// preview (tools/ramp_preview.cpp) renders the SAME layout and quantiser the
// board runs -- only the pixel sink differs. On the board the sink is FabGL's
// setRawPixel with the library's own createRawPixel LUT.
static void rampHwPut(void *ctx, int x, int y, uint8_t raw)
{
    (void)ctx;
    vga.setRawPixel(x, y, raw);
}

static void drawRamp()
{
    rampDraw(rampHwPut, nullptr, H_RES, V_RES);
}

// --- photo rendering --------------------------------------------------------

static void vgaPutPixel64(void *ctx, int x, int y,
                          int qr, int qg, int qb, const uint8_t *src)
{
    (void)ctx; (void)src;
    const int idx = (qb << 4) | (qg << 2) | qr;
    paletteUse[idx]++;
    vga.setRawPixel(x, y, rawLUT[idx]);
}

static void showPicture(const Picture &pic, int mode)
{
    const size_t bytes = (size_t)(pic.end - pic.start);
    memset(paletteUse, 0, sizeof(paletteUse));

    static uint8_t pool[TJPGD_WORKSPACE_SIZE] __attribute__((aligned(4)));
    RenderTarget64 target;
    memset(&target, 0, sizeof(target));
    target.stream.p       = pic.start;
    target.stream.left    = bytes;
    target.put           = vgaPutPixel64;
    target.ctx           = &vga;
    target.cfg.screenW   = H_RES;
    target.cfg.screenH   = V_RES;
    target.cfg.scaleShift = 1;      // 1/2 descale: 640x480 -> 320x240
    target.cfg.mode      = mode;

    JDEC jd;
    memset(&jd, 0, sizeof(jd));

    unsigned long t0 = millis();
    JRESULT res = jd_prepare(&jd, photo64InFunc, pool, sizeof(pool), &target);
    if (res != JDR_OK) {
        Serial.printf("ERROR: %s jd_prepare failed: %s (%d)\n",
                      pic.name, jdErrorString(res), (int)res);
        return;
    }

    res = jd_decomp(&jd, photo64OutFunc, target.cfg.scaleShift);
    if (res != JDR_OK) {
        Serial.printf("ERROR: %s jd_decomp failed: %s (%d)\n",
                      pic.name, jdErrorString(res), (int)res);
        return;
    }

    int used = 0;
    uint32_t minUse = 0xFFFFFFFFu, maxUse = 0;
    for (int i = 0; i < 64; i++) {
        if (paletteUse[i]) used++;
        if (paletteUse[i] < minUse) minUse = paletteUse[i];
        if (paletteUse[i] > maxUse) maxUse = paletteUse[i];
    }
    Serial.printf("photo %s: %u bytes embedded, %ux%u -> %dx%d (1/2 descale)\n",
                  pic.name, (unsigned)bytes, (unsigned)jd.width, (unsigned)jd.height,
                  H_RES, V_RES);
    Serial.printf("  drawn in %lu ms (%u pixels), mode=%s, free heap: %u bytes\n",
                  millis() - t0, (unsigned)target.pixelsDrawn,
                  mode == PHOTO64_MODE_PERCEPTUAL ? "perceptual" : "legacy",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    Serial.printf("  palette: %d of 64 colors used (counts %u..%u)\n",
                  used, (unsigned)minUse, (unsigned)maxUse);
}

// --- screen selection -------------------------------------------------------

// Returns the key pressed during the wait, or 0. 'q' cuts the wait short.
static char holdFor(unsigned long ms)
{
    const unsigned long t0 = millis();
    while (millis() - t0 < ms) {
        if (Serial.available()) {
            const char c = (char)Serial.read();
            if (c == 'q') return 0;
            if (c >= '1' && c <= '5') return c;
        }
        delay(20);
    }
    return 0;
}

static void selectScreen(char key)
{
    switch (key) {
        case '1':
            drawRamp();
            Serial.println("screen: diagnostic ramp (per-channel 0/LSB/MSB/both, grey ramp, gradient)");
            break;
        case '2': Serial.println("screen: lorikeet, perceptual pipeline");  showPicture(PIC_PARROT, PHOTO64_MODE_PERCEPTUAL); break;
        case '3': Serial.println("screen: blue-marble, perceptual pipeline"); showPicture(PIC_BLUE, PHOTO64_MODE_PERCEPTUAL); break;
        case '4': Serial.println("screen: lorikeet, round-1 (legacy) pipeline"); showPicture(PIC_PARROT, PHOTO64_MODE_LEGACY); break;
        case '5': Serial.println("screen: blue-marble, round-1 (legacy) pipeline"); showPicture(PIC_BLUE, PHOTO64_MODE_LEGACY); break;
        default: break;
    }
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("living-frame rung 2b -- 64-color diagnostic + two photos");
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

    photo64Init();

    float V[4];
    photo64Ladder(V);
    uint8_t code[4], lum[4];
    photo64LevelBytes(code);
    photo64LevelLuminance(lum);
    Serial.printf("dac levels (fraction of full scale): %.3f %.3f %.3f %.3f\n",
                  V[0], V[1], V[2], V[3]);
    Serial.printf("level sRGB code sent to DAC: %d %d %d %d\n",
                  code[0], code[1], code[2], code[3]);
    Serial.printf("level luminance as sRGB code: %d %d %d %d  <- step 1 is mid-grey\n",
                  lum[0], lum[1], lum[2], lum[3]);
    Serial.println("ramp order per channel, left to right: 0, LSB only, MSB only, both");

    drawRamp();
    Serial.println("screen: diagnostic ramp, held 30 s (power-cycle to see it again)");

    char k = holdFor(30000);
    if (k && k != '1') { selectScreen(k); return; }

    showPicture(PIC_PARROT, PHOTO64_MODE_PERCEPTUAL);
    Serial.println("holding lorikeet image; keys 1-5 switch screens, q cuts a wait short");
}

void loop()
{
    if (Serial.available()) {
        const char c = (char)Serial.read();
        if (c >= '1' && c <= '5') selectScreen(c);
    }
    delay(50);
}
