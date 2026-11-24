/* Which script Sin calls belong to a "Now * 0.1" stream? (see tweaks.c; also built into
 * the off-device test work/chk/calm_test.c)
 *
 * A stream is a sequence of calls, one per frame, whose argument grows by about 0.1 per
 * millisecond. The time between two calls is only known to a few milliseconds (the script
 * reads Now once per frame, the calls happen somewhere inside the frame), so a single step
 * is accepted loosely and the decision rests on the average over the whole streak. */
#ifndef TWEAKS_DETECT_H
#define TWEAKS_DETECT_H
#include <stdint.h>

#define CALM_RING 64
#define CALM_STREAMS 4
static struct { float x; uint64_t t; } calm_ring[CALM_RING];
static unsigned calm_ring_n;
static struct { float x0, x; uint64_t t0, t; int n; } calm_stream[CALM_STREAMS];

static int calm_step(float x, uint64_t now, float px, uint64_t pt) {
    double dt = (double)(now - pt) / 1e6;
    if (dt < 4.0 || dt > 70.0) return 0;
    double rate = ((double)x - (double)px) / dt;
    return rate > 0.07 && rate < 0.14;
}

/* 1 when this call (argument x at time now, in ns) is part of an established stream. */
static int calm_detect(float x, uint64_t now) {
    int calm = 0, taken = 0;
    for (int i = 0; i < CALM_STREAMS && !taken; i++) {
        if (!calm_stream[i].n) continue;
        if (now - calm_stream[i].t > 200000000ull) { calm_stream[i].n = 0; continue; }      /* ended */
        int same = x == calm_stream[i].x && now - calm_stream[i].t < 4000000ull;            /* same frame again */
        if (!same && !calm_step(x, now, calm_stream[i].x, calm_stream[i].t)) continue;
        if (!same) { calm_stream[i].x = x; calm_stream[i].t = now; calm_stream[i].n++; }
        double span = (double)(calm_stream[i].t - calm_stream[i].t0) / 1e6;
        double avg = span > 0 ? ((double)calm_stream[i].x - (double)calm_stream[i].x0) / span : 0;
        calm = calm_stream[i].n >= 4 && span >= 40.0 && avg > 0.092 && avg < 0.108;
        taken = 1;
    }
    if (!taken) {
        for (unsigned k = 0; k < CALM_RING && !taken; k++) {
            if (!calm_ring[k].t || !calm_step(x, now, calm_ring[k].x, calm_ring[k].t)) continue;
            int slot = 0;
            for (int i = 0; i < CALM_STREAMS; i++) {
                if (!calm_stream[i].n) { slot = i; break; }
                if (calm_stream[i].t < calm_stream[slot].t) slot = i;
            }
            calm_stream[slot].x0 = calm_ring[k].x; calm_stream[slot].t0 = calm_ring[k].t;
            calm_stream[slot].x = x; calm_stream[slot].t = now; calm_stream[slot].n = 2;
            taken = 1;
        }
    }
    calm_ring[calm_ring_n % CALM_RING].x = x; calm_ring[calm_ring_n % CALM_RING].t = now; calm_ring_n++;
    return calm;
}
#endif
