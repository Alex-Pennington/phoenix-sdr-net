/*******************************************************************************
 * FROZEN FILE - DO NOT MODIFY
 * See .github/copilot-instructions.md P1 section
 ******************************************************************************/

/**
 * @file waterfall_dsp.c
 * @brief DSP processing functions for waterfall display and audio
 *
 * Pure DSP functions - no globals, no side effects.
 * Display and audio paths use separate instances of these structs.
 */

#include "waterfall_dsp.h"

/*============================================================================
 * Lowpass Filter (2nd order Butterworth)
 *============================================================================*/

void wf_lowpass_init(wf_lowpass_t *lp, float cutoff_hz, float sample_rate) {
    /* 2nd order Butterworth lowpass */
    float w0 = 2.0f * 3.14159265f * cutoff_hz / sample_rate;
    float alpha = sinf(w0) / (2.0f * 0.7071f);  /* Q = 0.7071 for Butterworth */
    float cos_w0 = cosf(w0);

    float a0 = 1.0f + alpha;
    lp->b0 = (1.0f - cos_w0) / 2.0f / a0;
    lp->b1 = (1.0f - cos_w0) / a0;
    lp->b2 = (1.0f - cos_w0) / 2.0f / a0;
    lp->a1 = -2.0f * cos_w0 / a0;
    lp->a2 = (1.0f - alpha) / a0;

    lp->x1 = lp->x2 = 0.0f;
    lp->y1 = lp->y2 = 0.0f;
}

float wf_lowpass_process(wf_lowpass_t *lp, float x) {
    float y = lp->b0 * x + lp->b1 * lp->x1 + lp->b2 * lp->x2
            - lp->a1 * lp->y1 - lp->a2 * lp->y2;
    lp->x2 = lp->x1;
    lp->x1 = x;
    lp->y2 = lp->y1;
    lp->y1 = y;
    return y;
}

/*============================================================================
 * DC Removal (high-pass at ~3 Hz)
 *============================================================================*/

void wf_dc_block_init(wf_dc_block_t *dc) {
    dc->x_prev = 0.0f;
    dc->y_prev = 0.0f;
}

float wf_dc_block_process(wf_dc_block_t *dc, float x) {
    float y = x - dc->x_prev + 0.995f * dc->y_prev;
    dc->x_prev = x;
    dc->y_prev = y;
    return y;
}

/*============================================================================
 * Complete DSP chain for one path (display or audio)
 *============================================================================*/

void wf_dsp_path_init(wf_dsp_path_t *path, float cutoff_hz, float sample_rate) {
    wf_lowpass_init(&path->lowpass_i, cutoff_hz, sample_rate);
    wf_lowpass_init(&path->lowpass_q, cutoff_hz, sample_rate);
    wf_dc_block_init(&path->dc_block);
    path->initialized = true;
}

float wf_dsp_path_process(wf_dsp_path_t *path, float i_raw, float q_raw) {
    /* Lowpass filter I and Q */
    float i_filt = wf_lowpass_process(&path->lowpass_i, i_raw);
    float q_filt = wf_lowpass_process(&path->lowpass_q, q_raw);

    /* Compute magnitude (envelope) */
    float mag = sqrtf(i_filt * i_filt + q_filt * q_filt);

    /* DC block to remove carrier, keep modulation */
    float ac = wf_dc_block_process(&path->dc_block, mag);

    return ac;
}
