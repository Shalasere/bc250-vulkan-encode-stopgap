/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * rate_control.h - CBR/VBR/Low-Latency rate control header
 */
#ifndef RATE_CONTROL_H
#define RATE_CONTROL_H

#include <stdint.h>

typedef enum {
    RC_CBR,
    RC_VBR,
    RC_LOW_LATENCY  /* Low latency mode for Sunshine / Moonlight streaming */
} rc_mode_t;

typedef struct {
    rc_mode_t mode;
    uint32_t target_bitrate;
    uint32_t max_bitrate;
    int qp_min;
    int qp_max;

    /* Buffer model variables */
    double framerate;
    uint32_t target_bits_per_frame;
    int64_t buffer_fullness;
    int64_t buffer_size;
    int base_qp;
    int current_qp;
    uint64_t prev_frame_sad;
    int64_t error_integral;

    /* Wall-clock drain (see rc_update_stats): the leaky bucket used to drain
     * exactly target_bits_per_frame every frame, which silently enforces the
     * *negotiated* frame rate rather than the achieved one. On this hardware
     * a 1440p session negotiates 60 fps but the compute encoder sustains
     * ~40 fps, so a 60fps-sized per-frame budget delivered only ~2/3 of the
     * requested bitrate (measured: 20.91 Mbps against a 30.99 Mbps request,
     * with bits/frame matching target_bits_per_frame to 0.01% - the
     * controller was hitting its target perfectly, the target was just
     * sized for a frame rate that never arrives). Draining by real elapsed
     * time instead makes the bucket rate-correct at any achieved fps.
     * See docs/DEVLOG.md §16. */
    uint64_t last_frame_ns;   /* CLOCK_MONOTONIC of previous rc_update_stats; 0 = none yet */
    double   measured_fps;    /* EMA of achieved frame rate, diagnostics only */
} rate_control_t;

void rc_init(rate_control_t *rc, rc_mode_t mode, uint32_t bitrate, double fps,
             uint32_t width, uint32_t height);
int rc_get_frame_qp(rate_control_t *rc, uint64_t est_sad);
void rc_update_stats(rate_control_t *rc, int bits_used);

#endif
