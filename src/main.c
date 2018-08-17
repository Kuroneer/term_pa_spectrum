/*
   This file is part of the 'term_pa_spectrum' program, which follows
   a pulseaudio stream and displays its frequency spectrum through the
   terminal.

   Copyright (C) <2018> Jose Maria Perez Ramos

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.    See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.    If not, see <http://www.gnu.org/licenses/>.

   Author: Jose Maria Perez Ramos <jose.m.perez.ramos+git gmail>
   Date: 2018.08.13
   Version: 1.0.0
*/

#include "output.h"
#include "pulseaudio_follow_sink.h"
#include <complex.h>
#include <ctype.h>
#include <errno.h>
#include <fftw3.h>
#include <getopt.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

float timeSinceLastUpdate() {
    static struct timespec previous;
    struct timespec current;
    clock_gettime(CLOCK_MONOTONIC_RAW, &current);
    float delta_ms = ((float) (current.tv_sec - previous.tv_sec)) * 1000 + ((float) (current.tv_nsec - previous.tv_nsec)) / 1000000;

    previous.tv_sec = current.tv_sec;
    previous.tv_nsec = current.tv_nsec;
    return delta_ms;
}

// For options //
int atoi_exit_if_invalid(char* value, char option) {
    int ret = atoi(value);
    if (ret) {
        return ret;
    }

    fprintf(stderr, "Option `-%c' has invalid value <%s>\n", option, (value) ? value : "NULL");
    exit(1);
}

double atof_exit_if_invalid(char* value, char option) {
    double ret = atof(value);
    if (ret > 0.0) {
        return ret;
    }

    fprintf(stderr, "Option `-%c' has invalid value <%s>\n", option, (value) ? value : "NULL");
    exit(1);
}


typedef struct {char* s; int v;} var;
var charset_string2value[] = {
    {.s = "bars", .v = OUTPUT_CHARSET_BARS},
    {.s = "braille", .v = OUTPUT_CHARSET_BRAILLE},
    {.s = "wide_braille", .v = OUTPUT_CHARSET_BRAILLE_WIDE},
};
var grouping_string2value[] = {
    {.s = "none",   .v = OUTPUT_NO_GROUPING},
    {.s = "lineal", .v = OUTPUT_LINEAL_GROUPING},
    {.s = "log",    .v = OUTPUT_LOGARITMIC_GROUPING},
};
var groupingfunc_string2value[] = {
    {.s = "none", .v = OUTPUT_NO_GROUPING_FUNC},
    {.s = "max",  .v = OUTPUT_MAX_GROUPING_FUNC},
    {.s = "avg",  .v = OUTPUT_AVG_GROUPING_FUNC},
};
var transform_string2value[] = {
    {.s = "none", .v = OUTPUT_NO_TRANSFORM},
    {.s = "log",  .v = OUTPUT_LOGARITMIC_TRANSFORM},
};
var smoothing_string2value[] = {
    {.s = "none", .v = OUTPUT_NO_SMOOTH},
    {.s = "exp2", .v = OUTPUT_EXP2_SMOOTH},
};

int find_string_var(char* value, char option, var* values, int n_values) {
    for (int i = 0; i < n_values; ++i) {
        if (strcmp(value, values[i].s) == 0) {
            return values[i].v;
        }
    }

    fprintf (stderr, "Option `-%c' has invalid value <%s>\n", option, (value) ? value : "NULL");
    exit(1);
}
/////////////////

// CB INFO //////
typedef struct {
    float time_without_sound;
    unsigned int no_sound_wait_time_ms;
    unsigned int no_sound_sleep_time_ms;

    fftw_complex* fftw_out;
    fftw_plan plan;
    int n_out_values;

    double* graph;
    double* empty_graph;

    char new_line_char;
    unsigned int stats;
    void* out_ctx;
} cb_info_t;

int process_data_from_pa(int silence, void* userdata) {
    cb_info_t* cb_info = (cb_info_t*) userdata;
    char new_line_char = cb_info->new_line_char;
    float elapsed = timeSinceLastUpdate();

    ///////////////////
    // Input
    if (silence) {
        if (cb_info->time_without_sound > cb_info->no_sound_wait_time_ms) {
            fprintf(stdout, "%c%ls", new_line_char, output_print_silence(cb_info->out_ctx));
            fflush(stdout);
            return cb_info->no_sound_sleep_time_ms;
        }

        cb_info->time_without_sound += elapsed;
#ifdef DEBUG
        fprintf(stderr, "Silence for %3.0f ms", cb_info->time_without_sound);
#endif
        fprintf(stdout, "%c%ls", new_line_char, output_print(cb_info->out_ctx, cb_info->empty_graph));
        fflush(stdout);
        return 0;
    }
    cb_info->time_without_sound = 0;

    ///////////////////
    // Process data
    fftw_execute(cb_info->plan);

    for (int i = 0; i < cb_info->n_out_values; ++i) {
        fftw_complex c_out = cb_info->fftw_out[i];
        cb_info->graph[i] = sqrt(creal(c_out)*creal(c_out) + cimag(c_out)*cimag(c_out));
    }

    ///////////////////
    // Output
    // TODO Update output only based on fps
    fprintf(stdout, "%c%ls", new_line_char, output_print(cb_info->out_ctx, cb_info->graph));
    fflush(stdout);
#ifdef DEBUG
    fprintf(stderr,  "<%c", new_line_char);
    for (int i = 1; i < 41; ++i) {
        fprintf(stderr, "%4.0f ", cb_info->graph[i]/1000);
    }
#endif

    ///////////////////
    // Stats
    if (cb_info->stats) {
        fprintf(stdout, "> % 4.0f ms % 5.0f fps", elapsed, 1000/elapsed);
    }

    return 0;
}
/////////////////


int main(int argc, char **argv) {
    timeSinceLastUpdate();

    int n_samples = 1024; // n
    int sample_rate = 44100; // r
    int start_freq = 200; // f
    int end_freq = 2000;  // F // Not 4k because with low freq spikes it's difficult to see high freq ones

    int stats = 0; // s - print stats

    int no_sound_wait_time_ms = 3000;  // w - 3s without sound -> go to sleep
    int no_sound_sleep_time_ms = 5000; // W - Wake up every 5 seconds to check if there's sound

    int num_points = 30; // b
    int charset = OUTPUT_CHARSET_BARS; // c
    int grouping = OUTPUT_NO_GROUPING; // g
    int group_func = OUTPUT_MAX_GROUPING_FUNC; // G
    int transform = OUTPUT_NO_TRANSFORM; // t
    int smoothing = OUTPUT_EXP2_SMOOTH; // m
    double smooth_value_factor = .25;
    double smooth_limit_factor = .2;
    double lineal_scaling_factor_offset = .8; // o
    double sigmoid_scaling_factor = 0; // i
    char new_line_char = '\r'; // l

    char c;
    while ((c = getopt(argc, argv, "n:r:f:F:sw:W:b:c:g:G:t:m:o:i:hl")) != -1) {
        switch (c) {
            case 'n':
                n_samples = atoi_exit_if_invalid(optarg, 'n');
                break;
            case 'r':
                sample_rate = atoi_exit_if_invalid(optarg, 'r');
                break;
            case 'f':
                start_freq = atoi_exit_if_invalid(optarg, 'f');
                break;
            case 'F':
                end_freq = atoi_exit_if_invalid(optarg, 'F');
                break;
            case 's':
                stats = 1;
                break;
            case 'w':
                no_sound_wait_time_ms = atoi_exit_if_invalid(optarg, 'w');
                break;
            case 'W':
                no_sound_sleep_time_ms = atoi_exit_if_invalid(optarg, 'W');
                break;
            case 'b':
                num_points = atoi_exit_if_invalid(optarg, 'b');
                break;
            case 'c':
                charset = find_string_var(optarg, 'c', charset_string2value, sizeof(charset_string2value) / sizeof(var));
                break;
            case 'g':
                grouping = find_string_var(optarg, 'g', grouping_string2value, sizeof(grouping_string2value) / sizeof(var));
                break;
            case 'G':
                group_func = find_string_var(optarg, 'G', groupingfunc_string2value, sizeof(groupingfunc_string2value) / sizeof(var));
                break;
            case 't':
                transform = find_string_var(optarg, 't', transform_string2value, sizeof(transform_string2value) / sizeof(var));
                break;
            case 'm':
                smoothing = find_string_var(optarg, 'm', smoothing_string2value, sizeof(smoothing_string2value) / sizeof(var));
                break;
            case 'o':
                lineal_scaling_factor_offset = atof_exit_if_invalid(optarg, 'o');
                break;
            case 'i':
                sigmoid_scaling_factor = atof_exit_if_invalid(optarg, 'i');
                break;
            case 'l':
                new_line_char = '\n';
                break;
            case 'h':
                fprintf(stderr, "Available options:\n");
                fprintf(stderr, "-s: Show stats\n");
                fprintf(stderr, "-l: Use \\n as newline character\n");
                fprintf(stderr, "-b <%i>: Number of columns, only used if values are grouped\n", num_points);
                fprintf(stderr, "-c <bars>: Charset used to display values [bars, braille, wide_braille]\n");
                fprintf(stderr, "-g <none>: Grouping of values, none, lineal or logaritmic [none, lineal, log]\n");
                fprintf(stderr, "-G <none>: When grouping two or more values, how to do it [none, max, avg]\n");
                fprintf(stderr, "-t <none>: Transform values, either apply logaritmic function or not [none, log]\n");
                fprintf(stderr, "-m <exp2>: Smoothing [none, exp2]\n");
                fprintf(stderr, "-o <%f>: Apply lineal scaling factor offset\n", lineal_scaling_factor_offset);
                fprintf(stderr, "-i <%f>: Apply sigmoid function with factor (0 is disabled)\n", sigmoid_scaling_factor);
                fprintf(stderr, "-h: Show this help\n");
                fprintf(stderr, "Sleep options:\n");
                fprintf(stderr, "-w <%i>: After this time (ms), if no sound, the program goes to sleep\n", no_sound_wait_time_ms);
                fprintf(stderr, "-W <%i>: Wake up every X time to check if there's sound playing\n", no_sound_sleep_time_ms);
                fprintf(stderr, "Audio options:\n");
                fprintf(stderr, "-n <%i>: Audio buffer size\n", n_samples);
                fprintf(stderr, "-r <%i>: Audio sample rate\n", sample_rate);
                fprintf(stderr, "-f <%i>: min frequency\n", start_freq);
                fprintf(stderr, "-F <%i>: max frequency\n", end_freq);
                return 0;
        }
    }


    //// Init fftw
    double* amplitude_samples = (double*) fftw_malloc(sizeof(double) * n_samples);
    fftw_complex* fftw_out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * n_samples);
    fftw_plan plan = fftw_plan_dft_r2c_1d(n_samples, amplitude_samples, fftw_out, FFTW_MEASURE | FFTW_DESTROY_INPUT);
    int n_out_values = n_samples/2 +1;


    ///// Output freq
    double step_freq = ((double) sample_rate) / n_samples;
    // Freq = k * samples_per_second / buffer_size
    double* graph_freq = (double*) malloc(sizeof(double) * n_out_values);
    for (int i = 0; i < n_out_values; ++i) {
        graph_freq[i] = step_freq * i;
    }
#ifdef DEBUG
    for (int i = 1; i < 41; ++i) {
        fprintf(stderr, "%4.0f ", graph_freq[i]);
    }
    fprintf(stderr,  "<\n");
#endif


    //// Output buffers
    double* graph = (double*) malloc(sizeof(double) * n_out_values);
    double* empty_graph = (double*) calloc(n_out_values, sizeof(double));

    //// Print init
    output_context* out_ctx = output_init(
            n_out_values, // unsigned int data_length,
            graph_freq,   // double* data_frequency,
            start_freq,   // unsigned int min_freq,
            end_freq,     // unsigned int max_freq,
            num_points,   // unsigned int num_points,
            0,            // double abs_min,
            100000000,    // double abs_max,
            grouping,     // int grouping,
            group_func,   // int group_func,
            transform     // int transform flags
            );
    output_set_silence_str(out_ctx, L"No \u266C "); // No ♬ */
    output_set_smoothing(out_ctx, smoothing);
    output_set_smoothing_factors(out_ctx, smooth_value_factor, smooth_limit_factor);
    output_set_lineal_scale_factor_offset(out_ctx, lineal_scaling_factor_offset);
    if (sigmoid_scaling_factor > 0) {
        output_set_sigmoid_scale_factor(out_ctx, sigmoid_scaling_factor);
    }
    output_set_charset(out_ctx, charset);

    cb_info_t cb_info = {
        .time_without_sound = 0.0F,
        .no_sound_wait_time_ms = no_sound_wait_time_ms,
        .no_sound_sleep_time_ms = no_sound_sleep_time_ms,

        .fftw_out = fftw_out,
        .plan = plan,
        .n_out_values = n_out_values,

        .graph = graph,
        .empty_graph = empty_graph,

        .new_line_char = new_line_char,
        .stats = stats,
        .out_ctx = out_ctx,
    };

    //// Set up PA
    pa_set_up_read_callback(n_samples, sample_rate, amplitude_samples, process_data_from_pa, &cb_info);

    //// Free memory
    output_deinit(out_ctx);
    free(empty_graph);
    free(graph);
    free(graph_freq);
    fftw_free(fftw_out);
    fftw_free(amplitude_samples);

    return 0;
}

