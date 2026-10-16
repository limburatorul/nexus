/* The stretch of a desktop edge that screens touch: a 4K screen with a 1080p one to its right, tops aligned. */
#include <assert.h>
#include <stdio.h>
#include "../src/input.c"

int main(void)
{
    RECT screens[] = {{0, 0, 3840, 2160}, {3840, 0, 5760, 1080}}, v = {0, 0, 5760, 2160};
    int lo, hi;
    span_of(screens, 2, v, EDGE_RIGHT, &lo, &hi);
    assert(lo == 0 && hi == 1080); /* only the 1080p screen: 60% down it is y 648, not 1296 */
    assert(MulDiv(648 - lo, 65535, hi - lo - 1) * 100 / 65535 == 60);
    span_of(screens, 2, v, EDGE_LEFT, &lo, &hi);
    assert(lo == 0 && hi == 2160);
    span_of(screens, 2, v, EDGE_TOP, &lo, &hi);
    assert(lo == 0 && hi == 5760);
    span_of(screens, 2, v, EDGE_BOTTOM, &lo, &hi); /* the 1080p screen stops short of the desktop's bottom */
    assert(lo == 0 && hi == 3840);
    span_of(screens, 0, v, EDGE_RIGHT, &lo, &hi); /* no screens known: the whole desktop */
    assert(lo == 0 && hi == 2160);
    puts("edge_test: ok");
    return 0;
}
