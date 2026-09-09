/* bc250-vcn-driver v0.2.0 - https://github.com/Kai/bc250-vcn-driver */
/*
 * Copyright (c) 2026 BC-250 Project
 * SPDX-License-Identifier: MIT
 *
 * rate_control.c - Proportional-Integral CBR/VBR/Low-Latency rate controller
 */
#include "rate_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

/*
 * rc_estimate_base_qp - derive a starting QP from the requested bitrate and
 * resolution instead of a hardcoded constant.
 *
 * See docs/rate_control_audit.md sections 1 and 4 point 1: the old code set
 * base_qp = 26 unconditionally, so every QP the feedback loop could ever
 * reach (base_qp +- a handful of steps) was completely independent of what
 * bitrate was requested. This uses the standard, well-understood
 * bits-per-pixel <-> QP relationship real encoders (x264/x265's first-pass
 * QP guess included) rely on: H.264's quantization step size doubles every
 * 6 QP steps, so bits-per-pixel is roughly log-linear in QP - doubling the
 * bitrate at fixed resolution/framerate costs about -6 QP, and vice versa.
 *
 * The curve needs one calibration point to anchor it. Rather than invent
 * one, this reuses a real board measurement already on record
 * (docs/rate_control_audit.md SS3 Part C): a genuine CBR encode of 1280x720
 * @30fps synthetic "testsrc" content targeting 1 Mbps, under the OLD
 * hardcoded base_qp=26, converged to within -8.1% of that target - i.e.
 * QP ~26 empirically was already about right for ~0.036 bits/pixel of this
 * kind of content (see audit section 3, Part C). Anchoring here means the
 * one combination that was already known-good is left unchanged, and every
 * other resolution/bitrate combination scales off of a real measurement
 * instead of a guess.
 */
#define RC_QP_REF          26.0
#define RC_BPP_REF         0.036169   /* 1,000,000 / (1280*720*30) bits/pixel */
#define RC_QP_PER_DOUBLING 6.0        /* QP cost of one bitrate doubling */

static int rc_estimate_base_qp(uint32_t bitrate, double fps, uint32_t width, uint32_t height) {
    double pixels_per_sec = (double)width * (double)height * (fps > 0 ? fps : 60.0);
    if (pixels_per_sec <= 0.0) return (int)RC_QP_REF;

    double bpp = (double)bitrate / pixels_per_sec;
    if (bpp <= 0.0) return 51;

    double qp = RC_QP_REF - RC_QP_PER_DOUBLING * (log(bpp / RC_BPP_REF) / log(2.0));
    int qp_i = (int)lround(qp);
    if (qp_i < 12) qp_i = 12;
    if (qp_i > 51) qp_i = 51;
    return qp_i;
}

void rc_init(rate_control_t *rc, rc_mode_t mode, uint32_t bitrate, double fps,
             uint32_t width, uint32_t height) {
    if (!rc) return;
    rc->mode = mode;
    rc->target_bitrate = bitrate > 0 ? bitrate : 5000000;
    rc->max_bitrate = rc->target_bitrate * 3 / 2;
    rc->qp_min = 12;
    rc->qp_max = 51;
    rc->framerate = fps > 0 ? fps : 60.0;

    rc->target_bits_per_frame = (uint32_t)(rc->target_bitrate / rc->framerate);
    if (rc->target_bits_per_frame < 100) rc->target_bits_per_frame = 100;

    if (mode == RC_LOW_LATENCY) {
        /* 2-frame buffer for instant game streaming feedback */
        rc->buffer_size = rc->target_bits_per_frame * 2;
    } else {
        /* Standard 1-second leaky bucket buffer */
        rc->buffer_size = rc->target_bitrate;
    }
    if (rc->buffer_size < 1000) rc->buffer_size = 1000;

    rc->buffer_fullness = rc->buffer_size / 2;

    int base_qp = rc_estimate_base_qp(rc->target_bitrate, rc->framerate, width, height);
    rc->base_qp = base_qp;
    rc->current_qp = base_qp;
    rc->prev_frame_sad = 0;
    rc->error_integral = 0;
}

/*
 * Integral-term time constant and gain. This file's header comment has
 * always described a "Proportional-Integral" controller, and the struct
 * has always carried an error_integral field - but no code ever wrote or
 * read it, so in practice the loop was proportional-only, and (per
 * docs/rate_control_audit.md sections 1 and 4 point 2) p_term's normalized
 * error/target_level ratio is mathematically bounded to +-1, capping
 * qp_adjust to a fixed +-6 around base_qp no matter how large or
 * persistent the buffer error is.
 *
 * Wiring the integral term up lets *sustained* error - buffer error that
 * doesn't clear on its own within a handful of frames - keep pushing
 * current_qp further, toward the real qp_min/qp_max bounds (12/51), the
 * way a real CBR/VBR controller's long-term correction works, instead of
 * saturating at a fixed +-6 window forever. RC_INTEGRAL_WINDOW_FRAMES is
 * the number of frames of continuously-saturated error needed for the
 * integral term to reach its full RC_INTEGRAL_QP_RANGE contribution -
 * chosen as a multi-second settling window (independent of fps, since the
 * error unit here is already a per-frame buffer delta), similar in spirit
 * to the multi-second VBV windows real encoders use.
 */
#define RC_INTEGRAL_WINDOW_FRAMES 150.0
#define RC_INTEGRAL_QP_RANGE      40.0

int rc_get_frame_qp(rate_control_t *rc, uint64_t est_sad) {
    if (!rc) return 26;

    /* Compute buffer fullness deviation from 50% target */
    int64_t target_level = rc->buffer_size / 2;
    if (target_level <= 0) target_level = 1;
    int64_t error = rc->buffer_fullness - target_level;

    /* Proportional feedback: map buffer error to QP adjustments */
    double p_term = (double)error / (double)target_level * 6.0;

    /* Integral feedback: accumulate buffer error over time so a target that
     * is persistently unreachable within the proportional term's +-6 band
     * keeps walking current_qp further, instead of the loop giving up at a
     * fixed offset from base_qp forever. Anti-windup: stop accumulating in
     * a direction that's already saturated current_qp at qp_min/qp_max, so
     * the integral doesn't overshoot once the error eventually reverses. */
    int64_t integral_cap = (int64_t)(target_level * RC_INTEGRAL_WINDOW_FRAMES);
    if (integral_cap < 1) integral_cap = 1;
    bool saturated_high = (rc->current_qp >= rc->qp_max && error > 0);
    bool saturated_low  = (rc->current_qp <= rc->qp_min && error < 0);
    if (!saturated_high && !saturated_low) {
        rc->error_integral += error;
        if (rc->error_integral > integral_cap) rc->error_integral = integral_cap;
        if (rc->error_integral < -integral_cap) rc->error_integral = -integral_cap;
    }
    double i_term = ((double)rc->error_integral / (double)integral_cap) * RC_INTEGRAL_QP_RANGE;

    int qp_adjust = (int)round(p_term + i_term);

    /* VBR: Adjust for temporal complexity */
    if (rc->mode == RC_VBR && rc->prev_frame_sad > 0) {
        double complexity_ratio = (double)est_sad / (double)rc->prev_frame_sad;
        if (complexity_ratio > 1.3) qp_adjust += 2;
        else if (complexity_ratio < 0.7) qp_adjust -= 2;
    }

    /* Clamp maximum single-frame QP delta to prevent visual pulsation */
    int max_step = (rc->mode == RC_LOW_LATENCY) ? 3 : 2;
    int delta = (rc->base_qp + qp_adjust) - rc->current_qp;
    if (delta > max_step) delta = max_step;
    if (delta < -max_step) delta = -max_step;

    rc->current_qp += delta;

    if (rc->current_qp < rc->qp_min) rc->current_qp = rc->qp_min;
    if (rc->current_qp > rc->qp_max) rc->current_qp = rc->qp_max;

    rc->prev_frame_sad = est_sad;
    return rc->current_qp;
}

void rc_update_stats(rate_control_t *rc, int bits_used) {
    if (!rc) return;

    rc->buffer_fullness += bits_used;
    rc->buffer_fullness -= rc->target_bits_per_frame;

    if (rc->buffer_fullness < 0) {
        rc->buffer_fullness = 0;
    } else if (rc->buffer_fullness > rc->buffer_size) {
        rc->buffer_fullness = rc->buffer_size;
    }
}
