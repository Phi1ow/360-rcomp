// platform/ps5/pad_map.h: PS5 pad sample -> XINPUT state (host test).
#include <stdio.h>

#include "../ps5/pad_map.h"

static int g_fail = 0;
#define EXPECT(c)                                                     \
    do {                                                              \
        if (!(c)) {                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                 \
        }                                                             \
    } while (0)

int main() {
    using namespace rcomp::ps5pad;
    EXPECT(axis(128) == 0 && axis(255) == 32767 && axis(0) == -32768 && axis(129) > 0 && axis(127) < 0);
    EXPECT(axis_up(128) == 0 && axis_up(0) == 32767 && axis_up(255) == -32767);
    rcomp_pad p = from_sample(kCross | kCircle | kSquare | kTriangle | kOptions | kTouchPad | kL1 | kR1 | kL3 |
                                  kR3 | kUp | kDown | kLeft | kRight,
                              128, 0, 255, 128, 10, 250);
    EXPECT(p.buttons == 0xF3FF);  // every XINPUT button this runtime produces
    EXPECT(p.thumb_lx == 0 && p.thumb_ly == 32767 && p.thumb_rx == 32767 && p.thumb_ry == 0);
    EXPECT(p.left_trigger == 10 && p.right_trigger == 250);
    p = from_sample(kCross, 128, 128, 128, 128, 0, 0);
    EXPECT(p.buttons == RCOMP_XINPUT_A);
    p = from_sample(kCross | kIntercepted, 0, 0, 0, 0, 255, 255);  // shell owns the pad
    EXPECT(p.buttons == 0 && p.thumb_lx == 0 && p.left_trigger == 0);
    printf("pad_map: %s\n", g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
