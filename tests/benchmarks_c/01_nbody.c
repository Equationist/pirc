/*
 * Copyright (c) 2026 Ummon Karpe
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Classic 5-body solar system gravitational simulation (CLBG N-Body, pure C99). */
#include <math.h>
#include "bench_c_harness.h"

#define PI 3.141592653589793
#define SOLAR_MASS (4.0 * PI * PI)
#define DAYS_PER_YEAR 365.24

typedef struct {
    double x, y, z;
    double vx, vy, vz;
    double mass;
} Body;

static void advance(Body *bodies, int nbodies, double dt) {
    for (int i = 0; i < nbodies; ++i) {
        Body *b = &bodies[i];
        for (int j = i + 1; j < nbodies; ++j) {
            Body *b2 = &bodies[j];
            double dx = b->x - b2->x;
            double dy = b->y - b2->y;
            double dz = b->z - b2->z;
            double d2 = dx * dx + dy * dy + dz * dz;
            double dist = sqrt(d2);
            double mag = dt / (d2 * dist);
            b->vx -= dx * b2->mass * mag;
            b->vy -= dy * b2->mass * mag;
            b->vz -= dz * b2->mass * mag;
            b2->vx += dx * b->mass * mag;
            b2->vy += dy * b->mass * mag;
            b2->vz += dz * b->mass * mag;
        }
    }
    for (int i = 0; i < nbodies; ++i) {
        bodies[i].x += dt * bodies[i].vx;
        bodies[i].y += dt * bodies[i].vy;
        bodies[i].z += dt * bodies[i].vz;
    }
}

static double energy(const Body *bodies, int nbodies) {
    double e = 0.0;
    for (int i = 0; i < nbodies; ++i) {
        const Body *b = &bodies[i];
        e += 0.5 * b->mass * (b->vx * b->vx + b->vy * b->vy + b->vz * b->vz);
        for (int j = i + 1; j < nbodies; ++j) {
            const Body *b2 = &bodies[j];
            double dx = b->x - b2->x;
            double dy = b->y - b2->y;
            double dz = b->z - b2->z;
            double dist = sqrt(dx * dx + dy * dy + dz * dz);
            e -= (b->mass * b2->mass) / dist;
        }
    }
    return e;
}

static uint64_t bench_kernel(void) {
    Body bodies[5] = {
        { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, SOLAR_MASS },
        { 4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
          1.66007664274403694e-03 * DAYS_PER_YEAR, 7.69901118419740425e-03 * DAYS_PER_YEAR,
          -6.90460016972063023e-05 * DAYS_PER_YEAR, 9.54791938424326609e-04 * SOLAR_MASS },
        { 8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
          -2.76742510726862411e-03 * DAYS_PER_YEAR, 4.99852801234917238e-03 * DAYS_PER_YEAR,
          2.30417297573763929e-05 * DAYS_PER_YEAR, 2.85885980666130812e-04 * SOLAR_MASS },
        { 1.28943695621391310e+01, -1.51111514016986312e+01, -2.23307578892655734e-01,
          2.96460137564761618e-03 * DAYS_PER_YEAR, 2.37847173959480950e-03 * DAYS_PER_YEAR,
          -2.96589568540237556e-05 * DAYS_PER_YEAR, 4.36624404335156298e-05 * SOLAR_MASS },
        { 1.53796971148509165e+01, -2.59193146099879641e+01, 1.79258772950371181e-01,
          2.68067772490389322e-03 * DAYS_PER_YEAR, 1.62824170038242295e-03 * DAYS_PER_YEAR,
          -9.51592254519715870e-05 * DAYS_PER_YEAR, 5.15138902046611451e-05 * SOLAR_MASS }
    };

    double px = 0.0, py = 0.0, pz = 0.0;
    for (int i = 0; i < 5; ++i) {
        px += bodies[i].vx * bodies[i].mass;
        py += bodies[i].vy * bodies[i].mass;
        pz += bodies[i].vz * bodies[i].mass;
    }
    bodies[0].vx = -px / SOLAR_MASS;
    bodies[0].vy = -py / SOLAR_MASS;
    bodies[0].vz = -pz / SOLAR_MASS;

    for (int step = 0; step < 200000; ++step) {
        advance(bodies, 5, 0.01);
    }

    double e = energy(bodies, 5);
    /* Quantize to 1e-9 so FMA contraction differences don't cause false mismatch */
    int64_t q = (int64_t)(e * 1e9);
    return (uint64_t)q;
}
