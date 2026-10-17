/* The screens' edges and the push against them (input.c): staggered screens, corners, resistance,
 * the fade, the arrival grace. The layout is a 4K screen with a 1080p one to its right, tops aligned. */
#include <assert.h>
#include <stdio.h>
#include "../src/input.c"

static POINT pt(int x, int y)
{
    POINT p = {x, y};
    return p;
}

int main(void)
{
    Screens s = {{{0, 0, 3840, 2160}, {3840, 0, 5760, 1080}}, 2, 0};
    int lo, hi;

    /* Staggered: the 4K's right edge below the 1080p screen is a wall too, not only the desktop's. */
    assert(wall_at(&s, pt(5759, 500), EDGE_RIGHT));
    assert(wall_at(&s, pt(3839, 1500), EDGE_RIGHT));   /* nothing to the right of it there */
    assert(!wall_at(&s, pt(3839, 500), EDGE_RIGHT));   /* the 1080p screen continues there */
    assert(wall_at(&s, pt(4000, 1079), EDGE_BOTTOM));  /* the 1080p screen's bottom, above empty space */
    assert(!wall_at(&s, pt(4000, 1080), EDGE_BOTTOM)); /* off every screen */
    assert(!wall_at(&s, pt(5759, 500), EDGE_LEFT));
    /* Arrivals spread over the outermost screens, as in 0.4: only the 1080p one on the right, so 60% down it
     * is y 648, not 1296; leaving 60% down it says 60%. Leaving through the 4K's wall below it counts along
     * that stretch of wall: halfway down it is 50%. */
    span_of(&s, EDGE_RIGHT, &lo, &hi);
    assert(lo == 0 && hi == 1080);
    assert(along(&s, pt(5759, 648), EDGE_RIGHT) * 100 / 65535 == 60);
    assert(along(&s, pt(3839, 1620), EDGE_RIGHT) * 100 / 65535 == 50);
    span_of(&s, EDGE_LEFT, &lo, &hi);
    assert(lo == 0 && hi == 2160);
    span_of(&s, EDGE_BOTTOM, &lo, &hi); /* the 1080p screen stops short of the desktop's bottom */
    assert(lo == 0 && hi == 3840);
    span_of(&s, EDGE_TOP, &lo, &hi);
    assert(lo == 0 && hi == 5760);
    Screens none = {{{0, 0, 0, 0}}, 0, 0};
    span_of(&none, EDGE_RIGHT, &lo, &hi); /* no screens known: something sane */
    assert(lo < hi);
    /* Side by side, same height: the edge spans one screen, as before 0.5. */
    Screens even = {{{0, 0, 1920, 1080}, {1920, 0, 3840, 1080}}, 2, 0};
    span_of(&even, EDGE_RIGHT, &lo, &hi);
    assert(lo == 0 && hi == 1080);
    assert(along(&even, pt(3839, 648), EDGE_RIGHT) * 100 / 65535 == 60);

    /* Arrivals land just inside the outermost wall, clear of its ends. */
    POINT p = on_edge(&s, EDGE_RIGHT, 0);
    assert(p.x == 5757 && p.y == LANDING_CLEAR_PX);
    p = on_edge(&s, EDGE_RIGHT, 65535);
    assert(p.x == 5757 && p.y == 1079 - LANDING_CLEAR_PX);
    p = on_edge(&s, EDGE_BOTTOM, 49151); /* 75% along the bottom: on the 4K */
    assert(p.y == 2157 && p.x > 2800 && p.x < 2900);

    /* Corners: near either end of a wall a push does not count, mid-wall it does. */
    assert(in_corner(&s, pt(5759, 3), EDGE_RIGHT));
    assert(in_corner(&s, pt(5759, 1075), EDGE_RIGHT));
    assert(!in_corner(&s, pt(5759, 500), EDGE_RIGHT));
    assert(in_corner(&s, pt(3839, 1083), EDGE_RIGHT)); /* just below the 1080p screen: an inner corner */
    assert(!in_corner(&s, pt(3839, 1500), EDGE_RIGHT));

    /* Resistance: 120 px of push in quick steps gives; less does not. */
    Press pr = {0};
    DWORD t = 100000;
    assert(press_feed(&pr, TRUE, 50, t, 120) == PRESS_HOLD);
    assert(press_feed(&pr, TRUE, 50, t + 10, 120) == PRESS_HOLD);
    assert(press_feed(&pr, TRUE, 50, t + 20, 120) == PRESS_CROSS);
    assert(press_feed(&pr, TRUE, 50, t + 30, 120) == PRESS_HOLD); /* it starts again after giving */
    /* A slow lean fades away: 40 px every 200 ms never adds up to 120 (300 px/s fade). */
    memset(&pr, 0, sizeof pr);
    for (int i = 0; i < 20; i++) assert(press_feed(&pr, TRUE, 40, t + 1000 + i * 200, 120) == PRESS_HOLD);
    /* Moving back in takes the push away. */
    memset(&pr, 0, sizeof pr);
    press_feed(&pr, TRUE, 100, t, 120);
    assert(press_feed(&pr, FALSE, -90, t + 1, 120) == PRESS_PASS);
    assert(press_feed(&pr, TRUE, 100, t + 2, 120) == PRESS_HOLD);
    /* Off the wall a push counts for nothing; with no resistance a touch gives. */
    memset(&pr, 0, sizeof pr);
    assert(press_feed(&pr, FALSE, 500, t, 120) == PRESS_PASS);
    assert(press_feed(&pr, TRUE, 1, t + 1, 0) == PRESS_CROSS);
    /* Arrival grace: right after arriving the edge holds, even with no resistance... */
    press_arrive(&pr, t);
    assert(press_feed(&pr, TRUE, 500, t + 100, 0) == PRESS_HOLD);
    /* ...until 0.3 s have passed, */
    assert(press_feed(&pr, TRUE, 1, t + 400, 0) == PRESS_CROSS);
    /* or the pointer moved 4 px back in. */
    press_arrive(&pr, t);
    assert(press_feed(&pr, FALSE, -4, t + 10, 0) == PRESS_PASS);
    assert(press_feed(&pr, TRUE, 1, t + 20, 0) == PRESS_CROSS);

    puts("edge_test: ok");
    return 0;
}
