// ---------------------------------------------------------------------------
//  Living-frame rung 1 -- photo display firmware
//
//  Classic ESP32 (ESP32-WROOM-32 devkit) driving a real photographic JPEG
//  full-screen on a VGA monitor at 640x480.
//
//  Library: bitluni's ESP32Lib (VGA over I2S, 3-bit colour driver), reused in
//  place from ../vga-proof/lib/ESP32Lib so both rungs build identical source.
//  Decoder: TJpgDec R0.03 (elm-chan), vendored under lib/TJpgDec.
//  Render:  src/photo_render.h -- shared verbatim with tools/render_check.cpp
//           so the host check exercises the same decode/dither code.
//
//  Wiring is in ../vga-proof/WIRING.md (the proven 0138 map):
//    RED D14, GREEN D19, BLUE D27, HSYNC D32, VSYNC D33, GND GND.
//  Do not renumber -- they are the library's own classic-ESP32 PinConfig set.
//
//  WHY 3-BIT COLOUR: a 640x480 frame buffer is 614 kB at 14-bit and 307 kB at
//  6-bit; the classic ESP32 has 320 kB SRAM in total. The 3-bit driver packs
//  two pixels per byte (154 kB) and unpacks them in the I2S interrupt, so
//  640x480 is only reachable in 3-bit mode. One bit per channel is the
//  hardware limit of this path, so the image is ordered-dithered down to the
//  eight-colour palette rather than being a defect.
//
//  THE PHOTO IS IN FLASH, not on an SD card: data/photo.jpg is embedded by the
//  linker (board_build.embed_files in platformio.ini) and decoded from that
//  read-only buffer, MCU block by MCU block, straight into the frame buffer.
//  No filesystem, no WiFi, no SD hardware in this rung, and no full-image
//  buffer is ever allocated -- TJpgDec streams 16x16 (or 8x8/16x8) blocks.
// ---------------------------------------------------------------------------

#include <ESP32Video.h>
#include "photo_render.h"

// --- pin map (identical to ESP32Lib PinConfig VGAv01 / VGABlackEdition) ------
const int redPin    = 14;   // -> VGA R  (DB15 pin 1)
const int greenPin  = 19;   // -> VGA G  (DB15 pin 2)
const int bluePin   = 27;   // -> VGA B  (DB15 pin 3)
const int hsyncPin  = 32;   // -> VGA HSYNC (DB15 pin 13)
const int vsyncPin  = 33;   // -> VGA VSYNC (DB15 pin 14)

VGA3BitI vga;

static const int H_RES = 640;
static const int V_RES = 480;

// The embedded JPEG. PlatformIO's embed_files emits these linker symbols for
// data/photo.jpg; the name is derived from the path, not chosen by us.
extern "C" {
extern const uint8_t photo_jpg_start[] asm("_binary_data_photo_jpg_start");
extern const uint8_t photo_jpg_end[]   asm("_binary_data_photo_jpg_end");
}

// Pixel sink: one dithered pixel -> the VGA frame buffer.
static void vgaPutPixel(void *ctx, int x, int y, int r, int g, int b)
{
    VGA3BitI *v = (VGA3BitI *)ctx;
    v->dotFast(x, y, v->RGB(r, g, b));
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("living-frame rung 1 -- photo display");
    Serial.printf("pins: R=%d G=%d B=%d HSYNC=%d VSYNC=%d\n",
                  redPin, greenPin, bluePin, hsyncPin, vsyncPin);

    if (!vga.init(VGAMode::MODE640x480, redPin, greenPin, bluePin, hsyncPin, vsyncPin)) {
        Serial.println("ERROR: VGA init failed");
        while (true) delay(1000);
    }
    vga.clear(vga.RGB(0, 0, 0));

    const size_t jpegBytes = (size_t)(photo_jpg_end - photo_jpg_start);
    Serial.printf("photo.jpg embedded: %u bytes\n", (unsigned)jpegBytes);

    // TJpgDec workspace. 4-byte aligned: JD_FASTDECODE=1 uses 32-bit accesses.
    static uint8_t pool[TJPGD_WORKSPACE_SIZE] __attribute__((aligned(4)));

    RenderTarget target;
    memset(&target, 0, sizeof(target));
    target.stream.p    = photo_jpg_start;
    target.stream.left = jpegBytes;
    target.put         = vgaPutPixel;
    target.ctx         = &vga;
    target.screenW     = H_RES;
    target.screenH     = V_RES;

    JDEC jd;
    memset(&jd, 0, sizeof(jd));

    unsigned long t0 = millis();
    JRESULT res = jd_prepare(&jd, photoInFunc, pool, sizeof(pool), &target);
    if (res != JDR_OK) {
        Serial.printf("ERROR: jd_prepare failed: %s (%d)\n", jdErrorString(res), (int)res);
        while (true) delay(1000);
    }
    Serial.printf("jpeg: %ux%u, %u component(s) -> %ux%u screen\n",
                  (unsigned)jd.width, (unsigned)jd.height, (unsigned)jd.ncomp,
                  (unsigned)H_RES, (unsigned)V_RES);

    // 0 = no descaling. The source is authored at exactly 640x480, so any other
    // scale factor would either crop or letter-box the picture.
    res = jd_decomp(&jd, photoOutFunc, 0);
    if (res != JDR_OK) {
        Serial.printf("ERROR: jd_decomp failed: %s (%d)\n", jdErrorString(res), (int)res);
        while (true) delay(1000);
    }

    Serial.printf("frame drawn in %lu ms (%u pixels), free heap: %u bytes\n",
                  millis() - t0, (unsigned)target.pixelsDrawn,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
    Serial.println("holding image");
}

void loop()
{
    // The picture is a static frame held in the frame buffer; the I2S interrupt
    // keeps scanning it out. Nothing is redrawn, so what is on the screen is
    // stable by construction: no roll, no flicker, no tearing.
    delay(1000);
}
