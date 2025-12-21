/**
 * @file wormhole.c
 * @brief MIL-STD-188-110A Constellation Display
 *
 * Displays I/Q constellation for PSK signals.
 * Reads PCM files directly from tx_pcm_out directory and loops continuously.
 *
 * MIL-STD-188-110A Parameters (from Cm110s.h):
 *   - Sample Rate: 9600 Hz (modem internal), 48000 Hz (audio)
 *   - Symbol Rate: 2400 baud
 *   - Center Frequency: 1800 Hz
 *   - Samples per Symbol: 4 @ 9600 Hz, 20 @ 48000 Hz
 *
 * Usage: wormhole.exe [directory]
 *        Default directory: tx_pcm_out
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <dirent.h>

#include <SDL.h>
#include "version.h"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#endif

/*============================================================================
 * MIL-STD-188-110A Constants
 *============================================================================*/

#define SAMPLE_RATE         48000       /* Audio sample rate */
#define M110A_CENTER_HZ     1800.0f     /* Center frequency */
#define M110A_SYMBOL_RATE   2400        /* Symbols per second */
#define SAMPLES_PER_SYMBOL  (SAMPLE_RATE / M110A_SYMBOL_RATE)  /* 20 */

#define PI 3.14159265358979f
#define TWO_PI (2.0f * PI)

/*============================================================================
 * Display Configuration
 *============================================================================*/

#define WINDOW_SIZE     600             /* Square window */
#define GRID_DIVISIONS  8               /* Grid lines */
#define CONSTELLATION_RADIUS 250.0f     /* Pixels from center to unit circle */

/* Persistence/trail settings */
#define MAX_POINTS      500             /* Symbol history for persistence */
#define POINT_FADE      0.985f          /* How fast old points fade */

/*============================================================================
 * File Management
 *============================================================================*/

#define MAX_FILES       100
#define MAX_PATH_LEN    512

static char g_pcm_files[MAX_FILES][MAX_PATH_LEN];
static int g_num_files = 0;
static int g_current_file = 0;
static FILE *g_current_fp = NULL;
static char g_pcm_dir[MAX_PATH_LEN] = "tx_pcm_out";

static int scan_directory(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "Cannot open directory: %s\n", dir_path);
        return 0;
    }

    g_num_files = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && g_num_files < MAX_FILES) {
        const char *name = entry->d_name;
        size_t len = strlen(name);

        /* Check for .pcm extension */
        if (len > 4 && strcmp(name + len - 4, ".pcm") == 0) {
            snprintf(g_pcm_files[g_num_files], MAX_PATH_LEN, "%s/%s", dir_path, name);
            g_num_files++;
        }
    }
    closedir(dir);

    printf("Found %d PCM files in %s\n", g_num_files, dir_path);
    return g_num_files;
}

static FILE* open_next_file(void) {
    if (g_num_files == 0) return NULL;

    if (g_current_fp) {
        fclose(g_current_fp);
    }

    g_current_file = (g_current_file + 1) % g_num_files;
    g_current_fp = fopen(g_pcm_files[g_current_file], "rb");

    if (g_current_fp) {
        /* Extract just filename for display */
        const char *filename = strrchr(g_pcm_files[g_current_file], '/');
        if (!filename) filename = strrchr(g_pcm_files[g_current_file], '\\');
        if (filename) filename++; else filename = g_pcm_files[g_current_file];
        printf("\rPlaying: %s                    \n", filename);
    }

    return g_current_fp;
}

/*============================================================================
 * Matched Filter for Symbol Timing
 * Raised cosine pulse shaping per MIL-STD-188-110A
 *============================================================================*/

#define MATCHED_FILTER_LEN  41          /* Odd, centered */
static float g_matched_filter[MATCHED_FILTER_LEN];

static void init_matched_filter(void) {
    /* Raised cosine with alpha=0.35 (typical for 110A) */
    float alpha = 0.35f;
    int center = MATCHED_FILTER_LEN / 2;
    float T = (float)SAMPLES_PER_SYMBOL;  /* Symbol period in samples */

    for (int i = 0; i < MATCHED_FILTER_LEN; i++) {
        float t = (float)(i - center);
        float sinc_arg = t / T;
        float cos_arg = PI * alpha * t / T;

        /* sinc(t/T) */
        float sinc;
        if (fabsf(t) < 0.0001f) {
            sinc = 1.0f;
        } else {
            sinc = sinf(PI * sinc_arg) / (PI * sinc_arg);
        }

        /* cos(pi*alpha*t/T) / (1 - (2*alpha*t/T)^2) */
        float denom = 1.0f - 4.0f * alpha * alpha * sinc_arg * sinc_arg;
        float cosine_term;
        if (fabsf(denom) < 0.0001f) {
            cosine_term = PI / 4.0f;  /* Limit at singularity */
        } else {
            cosine_term = cosf(cos_arg) / denom;
        }

        g_matched_filter[i] = sinc * cosine_term;
    }

    /* Normalize */
    float sum = 0.0f;
    for (int i = 0; i < MATCHED_FILTER_LEN; i++) {
        sum += g_matched_filter[i] * g_matched_filter[i];
    }
    float norm = 1.0f / sqrtf(sum);
    for (int i = 0; i < MATCHED_FILTER_LEN; i++) {
        g_matched_filter[i] *= norm;
    }
}

/*============================================================================
 * Carrier Recovery (Costas Loop for PSK)
 *============================================================================*/

typedef struct {
    float phase;            /* Current NCO phase */
    float freq;             /* NCO frequency (radians per sample) */
    float freq_nominal;     /* Nominal carrier frequency */
    float alpha;            /* Loop filter - proportional gain */
    float beta;             /* Loop filter - integral gain */
} costas_loop_t;

static void costas_init(costas_loop_t *loop, float freq_hz, float bandwidth) {
    loop->phase = 0.0f;
    loop->freq_nominal = TWO_PI * freq_hz / SAMPLE_RATE;
    loop->freq = loop->freq_nominal;

    /* Loop bandwidth determines tracking speed vs noise */
    float damping = 0.707f;  /* Critically damped */
    float bw_norm = bandwidth / SAMPLE_RATE;
    float denom = 1.0f + 2.0f * damping * bw_norm + bw_norm * bw_norm;
    loop->alpha = 4.0f * damping * bw_norm / denom;
    loop->beta = 4.0f * bw_norm * bw_norm / denom;
}

static void costas_process(costas_loop_t *loop, float sample,
                           float *out_i, float *out_q, float *phase_error) {
    /* Mix down to baseband */
    float cos_p = cosf(loop->phase);
    float sin_p = sinf(loop->phase);

    *out_i = sample * cos_p * 2.0f;   /* 2x for mixer gain */
    *out_q = -sample * sin_p * 2.0f;

    /* Phase error detector for BPSK/QPSK */
    float error = (*out_i) * (*out_q);  /* Works for QPSK */

    /* Limit error to prevent wild swings */
    if (error > 1.0f) error = 1.0f;
    if (error < -1.0f) error = -1.0f;

    *phase_error = error;

    /* Update NCO */
    loop->freq += loop->beta * error;
    loop->phase += loop->freq + loop->alpha * error;

    /* Keep phase in [0, 2*pi) */
    while (loop->phase >= TWO_PI) loop->phase -= TWO_PI;
    while (loop->phase < 0.0f) loop->phase += TWO_PI;

    /* Limit frequency deviation (±50 Hz from nominal) */
    float max_dev = TWO_PI * 50.0f / SAMPLE_RATE;
    if (loop->freq > loop->freq_nominal + max_dev)
        loop->freq = loop->freq_nominal + max_dev;
    if (loop->freq < loop->freq_nominal - max_dev)
        loop->freq = loop->freq_nominal - max_dev;
}

/*============================================================================
 * Low-Pass Filter (for after mixing)
 *============================================================================*/

#define LPF_ORDER   4
typedef struct {
    float x[LPF_ORDER + 1];
    float y[LPF_ORDER + 1];
    float a[LPF_ORDER + 1];  /* Feedback coefficients */
    float b[LPF_ORDER + 1];  /* Feedforward coefficients */
} iir_filter_t;

static void lpf_init(iir_filter_t *f, float cutoff_hz) {
    memset(f, 0, sizeof(*f));

    /* Simple 2nd-order Butterworth approximation */
    float fc = cutoff_hz / SAMPLE_RATE;
    float w = tanf(PI * fc);
    float w2 = w * w;
    float r = sqrtf(2.0f);
    float d = w2 + r * w + 1.0f;

    f->b[0] = w2 / d;
    f->b[1] = 2.0f * w2 / d;
    f->b[2] = w2 / d;
    f->a[0] = 1.0f;
    f->a[1] = 2.0f * (w2 - 1.0f) / d;
    f->a[2] = (w2 - r * w + 1.0f) / d;
}

static float lpf_process(iir_filter_t *f, float in) {
    /* Shift input history */
    for (int i = LPF_ORDER; i > 0; i--) {
        f->x[i] = f->x[i-1];
        f->y[i] = f->y[i-1];
    }
    f->x[0] = in;

    /* IIR filter */
    float out = f->b[0] * f->x[0] + f->b[1] * f->x[1] + f->b[2] * f->x[2]
              - f->a[1] * f->y[1] - f->a[2] * f->y[2];
    f->y[0] = out;

    return out;
}

/*============================================================================
 * AGC (Automatic Gain Control)
 *============================================================================*/

typedef struct {
    float gain;
    float target;       /* Target RMS level */
    float attack;       /* Attack rate */
    float decay;        /* Decay rate */
} agc_t;

static void agc_init(agc_t *agc, float target) {
    agc->gain = 1.0f;
    agc->target = target;
    agc->attack = 0.01f;
    agc->decay = 0.0001f;
}

static void agc_process(agc_t *agc, float *i, float *q) {
    float mag = sqrtf((*i)*(*i) + (*q)*(*q));

    if (mag > 0.001f) {
        float error = agc->target - mag * agc->gain;
        if (error > 0) {
            agc->gain += agc->decay * error;
        } else {
            agc->gain += agc->attack * error;
        }

        /* Limit gain */
        if (agc->gain < 0.01f) agc->gain = 0.01f;
        if (agc->gain > 100.0f) agc->gain = 100.0f;
    }

    *i *= agc->gain;
    *q *= agc->gain;
}

/*============================================================================
 * Constellation Point Storage
 *============================================================================*/

typedef struct {
    float i, q;
    float brightness;   /* 1.0 = new, fades to 0 */
} constellation_point_t;

static constellation_point_t g_points[MAX_POINTS];
static int g_point_head = 0;
static int g_point_count = 0;

static void add_constellation_point(float i, float q) {
    g_points[g_point_head].i = i;
    g_points[g_point_head].q = q;
    g_points[g_point_head].brightness = 1.0f;

    g_point_head = (g_point_head + 1) % MAX_POINTS;
    if (g_point_count < MAX_POINTS) g_point_count++;
}

static void fade_points(void) {
    for (int i = 0; i < g_point_count; i++) {
        g_points[i].brightness *= POINT_FADE;
    }
}

/*============================================================================
 * Drawing Functions
 *============================================================================*/

static void draw_grid(uint8_t *pixels) {
    int cx = WINDOW_SIZE / 2;
    int cy = WINDOW_SIZE / 2;

    /* Background */
    memset(pixels, 15, WINDOW_SIZE * WINDOW_SIZE * 3);

    /* Draw grid lines */
    for (int g = 0; g <= GRID_DIVISIONS; g++) {
        int offset = (int)(g * CONSTELLATION_RADIUS * 2 / GRID_DIVISIONS - CONSTELLATION_RADIUS);

        /* Vertical line */
        int x = cx + offset;
        if (x >= 0 && x < WINDOW_SIZE) {
            for (int y = cy - (int)CONSTELLATION_RADIUS; y <= cy + (int)CONSTELLATION_RADIUS; y++) {
                if (y >= 0 && y < WINDOW_SIZE) {
                    int idx = (y * WINDOW_SIZE + x) * 3;
                    uint8_t color = (g == GRID_DIVISIONS/2) ? 60 : 35;
                    pixels[idx] = color;
                    pixels[idx+1] = color;
                    pixels[idx+2] = color;
                }
            }
        }

        /* Horizontal line */
        int y2 = cy + offset;
        if (y2 >= 0 && y2 < WINDOW_SIZE) {
            for (int x2 = cx - (int)CONSTELLATION_RADIUS; x2 <= cx + (int)CONSTELLATION_RADIUS; x2++) {
                if (x2 >= 0 && x2 < WINDOW_SIZE) {
                    int idx = (y2 * WINDOW_SIZE + x2) * 3;
                    uint8_t color = (g == GRID_DIVISIONS/2) ? 60 : 35;
                    pixels[idx] = color;
                    pixels[idx+1] = color;
                    pixels[idx+2] = color;
                }
            }
        }
    }

    /* Draw unit circle */
    float unit_radius = CONSTELLATION_RADIUS * 0.7f;
    for (int angle = 0; angle < 360; angle++) {
        float rad = angle * PI / 180.0f;
        int x = cx + (int)(unit_radius * cosf(rad));
        int y = cy - (int)(unit_radius * sinf(rad));
        if (x >= 0 && x < WINDOW_SIZE && y >= 0 && y < WINDOW_SIZE) {
            int idx = (y * WINDOW_SIZE + x) * 3;
            pixels[idx] = 50;
            pixels[idx+1] = 50;
            pixels[idx+2] = 70;
        }
    }

    /* Draw ideal 8-PSK constellation points */
    for (int n = 0; n < 8; n++) {
        float angle = n * PI / 4.0f;
        int x = cx + (int)(unit_radius * cosf(angle));
        int y = cy - (int)(unit_radius * sinf(angle));

        /* Draw cross marker */
        for (int d = -4; d <= 4; d++) {
            if (x+d >= 0 && x+d < WINDOW_SIZE) {
                int idx = (y * WINDOW_SIZE + x + d) * 3;
                pixels[idx] = 80; pixels[idx+1] = 80; pixels[idx+2] = 100;
            }
            if (y+d >= 0 && y+d < WINDOW_SIZE) {
                int idx = ((y+d) * WINDOW_SIZE + x) * 3;
                pixels[idx] = 80; pixels[idx+1] = 80; pixels[idx+2] = 100;
            }
        }
    }
}

static void draw_points(uint8_t *pixels) {
    int cx = WINDOW_SIZE / 2;
    int cy = WINDOW_SIZE / 2;
    float scale = CONSTELLATION_RADIUS * 0.7f;

    for (int p = 0; p < g_point_count; p++) {
        constellation_point_t *pt = &g_points[p];
        if (pt->brightness < 0.05f) continue;

        int x = cx + (int)(pt->i * scale);
        int y = cy - (int)(pt->q * scale);

        /* Color based on phase angle */
        float phase = atan2f(pt->q, pt->i);
        float hue = (phase + PI) / TWO_PI;

        /* Simple HSV to RGB */
        float h = hue * 6.0f;
        int hi = (int)h % 6;
        float f = h - (int)h;
        float v = pt->brightness;
        float t = v * f;
        float s2 = v * (1.0f - f);

        float r, g, b;
        switch (hi) {
            case 0: r = v; g = t; b = 0; break;
            case 1: r = s2; g = v; b = 0; break;
            case 2: r = 0; g = v; b = t; break;
            case 3: r = 0; g = s2; b = v; break;
            case 4: r = t; g = 0; b = v; break;
            default: r = v; g = 0; b = s2; break;
        }

        /* Draw point (3x3 for visibility) */
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int px = x + dx;
                int py = y + dy;
                if (px >= 0 && px < WINDOW_SIZE && py >= 0 && py < WINDOW_SIZE) {
                    int idx = (py * WINDOW_SIZE + px) * 3;
                    int nr = pixels[idx] + (int)(r * 200);
                    int ng = pixels[idx+1] + (int)(g * 200);
                    int nb = pixels[idx+2] + (int)(b * 200);
                    pixels[idx] = (nr > 255) ? 255 : nr;
                    pixels[idx+1] = (ng > 255) ? 255 : ng;
                    pixels[idx+2] = (nb > 255) ? 255 : nb;
                }
            }
        }
    }
}

/*============================================================================
 * Main
 *============================================================================*/

int main(int argc, char *argv[]) {
    print_version("Phoenix SDR - Wormhole (M110A Constellation)");

    /* Parse command line */
    if (argc > 1) {
        strncpy(g_pcm_dir, argv[1], MAX_PATH_LEN - 1);
    }

    /* Scan for PCM files */
    if (scan_directory(g_pcm_dir) == 0) {
        fprintf(stderr, "No PCM files found in %s\n", g_pcm_dir);
        return 1;
    }

    /* Initialize DSP blocks */
    init_matched_filter();

    costas_loop_t carrier;
    costas_init(&carrier, M110A_CENTER_HZ, 50.0f);

    iir_filter_t lpf_i, lpf_q;
    lpf_init(&lpf_i, 1500.0f);
    lpf_init(&lpf_q, 1500.0f);

    agc_t agc;
    agc_init(&agc, 0.7f);

    /* Initialize SDL */
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow(
        "Wormhole - MIL-STD-188-110A Constellation",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WINDOW_SIZE, WINDOW_SIZE,
        SDL_WINDOW_SHOWN
    );
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_Texture *texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
        WINDOW_SIZE, WINDOW_SIZE
    );
    if (!texture) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    uint8_t *pixels = (uint8_t *)malloc(WINDOW_SIZE * WINDOW_SIZE * 3);
    if (!pixels) {
        fprintf(stderr, "Memory allocation failed\n");
        return 1;
    }

    printf("M110A: %d baud, %.0f Hz center, %d samples/symbol\n",
           M110A_SYMBOL_RATE, M110A_CENTER_HZ, SAMPLES_PER_SYMBOL);
    printf("\nControls:\n");
    printf("  C: Clear constellation\n");
    printf("  N: Next file\n");
    printf("  +/-: Adjust AGC target\n");
    printf("  Q/Esc: Quit\n\n");

    /* Open first file */
    g_current_file = -1;  /* Will wrap to 0 */
    open_next_file();

    bool running = true;
    int sample_counter = 0;
    int symbols_received = 0;

    float decim_i[SAMPLES_PER_SYMBOL];
    float decim_q[SAMPLES_PER_SYMBOL];
    int decim_idx = 0;

    while (running) {
        /* Handle SDL events */
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                    case SDLK_ESCAPE:
                    case SDLK_q:
                        running = false;
                        break;
                    case SDLK_c:
                        g_point_count = 0;
                        g_point_head = 0;
                        printf("Constellation cleared\n");
                        break;
                    case SDLK_n:
                        open_next_file();
                        break;
                    case SDLK_PLUS:
                    case SDLK_EQUALS:
                        agc.target *= 1.2f;
                        printf("AGC target: %.2f\n", agc.target);
                        break;
                    case SDLK_MINUS:
                        agc.target /= 1.2f;
                        printf("AGC target: %.2f\n", agc.target);
                        break;
                }
            }
        }

        /* Read samples from current file */
        int16_t pcm_block[256];
        size_t samples_read = 0;

        if (g_current_fp) {
            samples_read = fread(pcm_block, sizeof(int16_t), 256, g_current_fp);
        }

        if (samples_read == 0) {
            /* End of file - open next one */
            if (!open_next_file()) {
                /* No files available, wait a bit */
                SDL_Delay(100);
                continue;
            }
            samples_read = fread(pcm_block, sizeof(int16_t), 256, g_current_fp);
        }

        /* Process samples */
        for (size_t s = 0; s < samples_read; s++) {
            float sample = pcm_block[s] / 32768.0f;

            /* Carrier recovery and downconversion */
            float raw_i, raw_q, phase_err;
            costas_process(&carrier, sample, &raw_i, &raw_q, &phase_err);

            /* Low-pass filter */
            float filt_i = lpf_process(&lpf_i, raw_i);
            float filt_q = lpf_process(&lpf_q, raw_q);

            /* Accumulate for symbol decimation */
            decim_i[decim_idx] = filt_i;
            decim_q[decim_idx] = filt_q;
            decim_idx++;

            sample_counter++;

            /* At symbol boundary, sample the constellation */
            if (decim_idx >= SAMPLES_PER_SYMBOL) {
                int opt_idx = SAMPLES_PER_SYMBOL / 2;
                float sym_i = decim_i[opt_idx];
                float sym_q = decim_q[opt_idx];

                agc_process(&agc, &sym_i, &sym_q);
                add_constellation_point(sym_i, sym_q);
                symbols_received++;

                decim_idx = 0;
            }
        }

        /* Fade old points */
        fade_points();

        /* Draw */
        draw_grid(pixels);
        draw_points(pixels);

        SDL_UpdateTexture(texture, NULL, pixels, WINDOW_SIZE * 3);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        /* Status update */
        if (sample_counter % 48000 < 256) {
            float freq_offset = (carrier.freq - carrier.freq_nominal) * SAMPLE_RATE / TWO_PI;
            printf("\rSymbols: %d  AGC: %.2f  Freq: %+.1f Hz  File: %d/%d  ",
                   symbols_received, agc.gain, freq_offset,
                   g_current_file + 1, g_num_files);
            fflush(stdout);
        }
    }

    printf("\n\nShutting down...\n");

    if (g_current_fp) fclose(g_current_fp);
    free(pixels);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
