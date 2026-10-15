// ---------------------------------------------------------------------------
//  Living-frame rung 0 -- VGA proof firmware
//
//  Classic ESP32 (ESP32-WROOM-32 devkit or DFRobot FireBeetle ESP32-E)
//  driving a VGA test pattern on the Magnavox 22MD311B/F7 through its
//  VGA / PC input.
//
//  Library: bitluni's ESP32Lib (VGA over I2S, 3-bit colour driver).
//  Mode:    640x480 @ 25.175 MHz pixel clock, 8 colours (1 bit per channel),
//           which is exactly what an 8-bar colour-bar pattern needs.
//
//  Wiring is in WIRING.md.  The five signal pins below are the ONLY pins that
//  matter; they are the library's standard classic-ESP32 set (PinConfig
//  VGAv01 / VGABlackEdition), repeated here as named constants so the
//  firmware and the wiring table cannot drift apart.
//
//  NOTE ON COLOUR DEPTH: 640x480 in 6-bit or 14-bit colour needs a frame
//  buffer of 307-614 kB, which does not fit in the ESP32's 320 kB SRAM.  The
//  3-bit driver packs 2 pixels per byte (153 kB at 640x480) and unpacks them
//  in the I2S interrupt, so 640x480 is only reachable in 3-bit mode.  Eight
//  colours is the right trade for a colour-bar proof.
// ---------------------------------------------------------------------------

#include <ESP32Video.h>
#include <Ressources/CodePage437_8x19.h>   // 8x19 px font, readable on a 22" TV

// --- pin map (identical to ESP32Lib PinConfig VGAv01 / VGABlackEdition) ------
const int redPin    = 14;   // -> VGA R  (red channel)
const int greenPin  = 19;   // -> VGA G  (green channel)
const int bluePin   = 27;   // -> VGA B  (blue channel)
const int hsyncPin  = 32;   // -> VGA HSYNC (pin 13 on the DB15)
const int vsyncPin  = 33;   // -> VGA VSYNC (pin 14 on the DB15)

VGA3BitI vga;

static const int H_RES = 640;
static const int V_RES = 480;

// EBU/SMPTE-style 8-bar pattern, left to right, at 100 % level.
// (3-bit RGB can produce exactly these eight.)
static const uint32_t BARS[8] = {
    0xffffff,  // white
    0xffff00,  // yellow
    0x00ffff,  // cyan
    0x00ff00,  // green
    0xff00ff,  // magenta
    0xff0000,  // red
    0x0000ff,  // blue
    0x000000   // black
};

static const char *LABEL_LINE_1 = "NORDTRONICS  LIVING-FRAME";
static const char *LABEL_LINE_2 = "VGA PROOF  -  640x480  -  ESP32";

// Centre a string of the 8x19 font horizontally.
static void centredText(const char *s, int y, int charWidth = 8, int screenW = H_RES)
{
    int len = strlen(s);
    int x = (screenW - len * charWidth) / 2;
    if (x < 0) x = 0;
    vga.setCursor(x, y);
    vga.print(s);
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("living-frame rung 0 -- VGA proof");
    Serial.printf("pins: R=%d G=%d B=%d HSYNC=%d VSYNC=%d\n",
                  redPin, greenPin, bluePin, hsyncPin, vsyncPin);

    // 640x480, 3-bit colour, single frame buffer.
    if (!vga.init(VGAMode::MODE640x480, redPin, greenPin, bluePin, hsyncPin, vsyncPin))
    {
        Serial.println("ERROR: VGA init failed");
        while (true) delay(1000);
    }
    vga.setFont(CodePage437_8x19);

    // ---- colour bars -------------------------------------------------------
    const int barW = H_RES / 8;        // 80 px
    const int barH = V_RES * 3 / 4;    // 360 px -- top three quarters
    for (int i = 0; i < 8; i++)
    {
        uint32_t c = BARS[i];
        vga.fillRect(i * barW, 0, barW, barH,
                     vga.RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff));
    }

    // ---- label band --------------------------------------------------------
    const int bandY = barH;            // 360
    const int bandH = V_RES - barH;    // 120 px
    vga.fillRect(0, bandY, H_RES, bandH, vga.RGB(32, 32, 32));

    // thin white rules top and bottom of the band
    vga.fillRect(0, bandY, H_RES, 3, vga.RGB(255, 255, 255));
    vga.fillRect(0, V_RES - 3, H_RES, 3, vga.RGB(255, 255, 255));

    vga.setTextColor(vga.RGB(255, 255, 255), vga.RGB(32, 32, 32));
    centredText(LABEL_LINE_1, bandY + 22);
    centredText(LABEL_LINE_2, bandY + 22 + 30);

    // ---- alignment marks in the four corners -------------------------------
    // If these are clipped, the TV is not displaying the full 640x480 frame
    // (check the TV's aspect/zoom setting -- it should be "Just scan" / 1:1).
    const int m = 12;
    vga.fillRect(0, 0, m, m, vga.RGB(255, 255, 255));
    vga.fillRect(H_RES - m, 0, m, m, vga.RGB(255, 255, 255));
    vga.fillRect(0, barH - m, m, m, vga.RGB(255, 255, 255));
    vga.fillRect(H_RES - m, barH - m, m, m, vga.RGB(255, 255, 255));

    Serial.printf("frame drawn, free heap: %u bytes\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
}

void loop()
{
    // The picture is a static frame held in the single frame buffer; the I2S
    // interrupt keeps scanning it out.  Nothing to do here, and nothing is
    // redrawn, so what is on the screen is stable by construction.
    delay(1000);
}
