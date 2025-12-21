/*******************************************************************************
 * FROZEN FILE - DO NOT MODIFY
 * See .github/copilot-instructions.md P1 section
 ******************************************************************************/

/**
 * @file waterfall_dsp.h
 * @brief DSP processing functions for waterfall display and audio
 *
 * Pure DSP functions - no globals, no side effects.
 * Display and audio paths use separate instances of these structs.
 */

#ifndef WATERFALL_DSP_H
#define WATERFALL_DSP_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/*============================================================================
 * Lowpass Filter (2nd order Butterworth)
 *============================================================================*/

typedef struct {
    float x1, x2;   /* Input history */
    float y1, y2;   /* Output history */
    float b0, b1, b2, a1, a2;  /* Coefficients */
} wf_lowpass_t;

void wf_lowpass_init(wf_lowpass_t *lp, float cutoff_hz, float sample_rate);
float wf_lowpass_process(wf_lowpass_t *lp, float x);

/*============================================================================
 * DC Removal (high-pass at ~3 Hz)
 *============================================================================*/

typedef struct {
    float x_prev;
    float y_prev;
} wf_dc_block_t;

void wf_dc_block_init(wf_dc_block_t *dc);
float wf_dc_block_process(wf_dc_block_t *dc, float x);

/*============================================================================
 * Complete DSP chain for one path (display or audio)
 *============================================================================*/

typedef struct {
    wf_lowpass_t lowpass_i;
    wf_lowpass_t lowpass_q;
    wf_dc_block_t dc_block;
    bool initialized;
} wf_dsp_path_t;

void wf_dsp_path_init(wf_dsp_path_t *path, float cutoff_hz, float sample_rate);
float wf_dsp_path_process(wf_dsp_path_t *path, float i_raw, float q_raw);

#endif /* WATERFALL_DSP_H */
