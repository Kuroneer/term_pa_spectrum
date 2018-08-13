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

// This file groups, smooths and maps to the output chars, nothing else
#include "output.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define max(a,b) \
    ({ __typeof__ (a) _a = (a); \
     __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

#define min(a,b) \
    ({ __typeof__ (a) _a = (a); \
     __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })


wchar_t bars_str[] =
L" \u2581\u2582\u2583\u2584\u2585\u2586\u2587\u2588" // " ▁▂▃▄▅▆▇█"
;
unsigned int bars_points_per_char = 1;
unsigned int bars_levels = 9;


wchar_t braille_str[] =
     L" \u2880\u28A0\u28B0\u28B8" // " ⢀⢠⢰⢸"
L"\u2840\u28C0\u28E0\u28F0\u28F8" // "⡀⣀⣠⣰⣸"
L"\u2844\u28C4\u28E4\u28F4\u28FC" // "⡄⣄⣤⣴⣼"
L"\u2846\u28C6\u28E6\u28F6\u28FE" // "⡆⣆⣦⣶⣾"
L"\u2847\u28C7\u28E7\u28F7\u28FF" // "⡇⣇⣧⣷⣿"
;
unsigned int braille_points_per_char = 2;
unsigned int braille_levels = 5;

wchar_t* silence_str = L" No data ";

struct output_context {
    unsigned int* data_buffer_index_to_acc_buffer_index; // Data buffer index -> Acc buffer index relationship
    unsigned int* acc_buffer_data_count;                 // Acc buffer data values count by position
    double* acc_buffer_avg_factor;
    unsigned int min_data_index;                         // Min relevant data buffer index
    unsigned int max_data_index;                         // Max relevant data buffer index
    unsigned int num_points;                             // Number of points to be displayed (length of acc buffer and related buffers)
    double abs_min;                                      // Values lower than this are mapped to the min value
    double abs_max;                                      // Values higher than this are mapped to the max value
    int group_func;                                      // MAX/AVG
    int smoothing;                                       // Smoothing strategy
    double smoothing_new_value_factor;                   // Smoothing factor for new values
    double smoothing_old_value_factor;                   // Smoothing factor for old values
    double smoothing_new_limit_factor;                   // Smoothing factor for new limit
    double smoothing_old_limit_factor;                   // Smoothing factor for old limit
    double smoothing_min_limit;
    double smoothing_max_limit;
    unsigned int transform_flags;                        // Transform function

    double sigmoid_scaling_factor;                       // Apply sigmoid to the output
    double lineal_scaling_factor;                        // Scale results to see better the peaks

    unsigned int visualization_levels;
    unsigned int visualization_points_per_char;
    wchar_t* visualization_str;

    double* acc_buffer;                                  // Intermediate acc buffers
    double* smooth_buffer;
    wchar_t* wchar_buffer;

    wchar_t* provided_silence_str;
    wchar_t* wchar_silence_buffer;
};


output_context* output_init(
        unsigned int data_length,
        double* data_frequency,
        unsigned int min_freq,
        unsigned int max_freq,
        unsigned int num_points,
        double abs_min,
        double abs_max,
        int group,               // No/Logaritmic/Lineal
        int group_func,          // MAX/AVG
        unsigned int transform_flags
        ) {

    setlocale(LC_ALL, "");

    output_context* out_ctx = NULL;
    if ((out_ctx = malloc(sizeof *out_ctx))) {
        unsigned int i;
        int no_grouping = 0;

        switch ((group_func && group) ? group : OUTPUT_NO_GROUPING) {
            case OUTPUT_LOGARITMIC_GROUPING:
                min_freq = log(min_freq);
                max_freq = log(max_freq);
                for (i = 0; i < data_length; ++i) {
                    data_frequency[i] = log(data_frequency[i]);
                }
                break;
            case OUTPUT_LINEAL_GROUPING:
                break;
            case OUTPUT_NO_GROUPING:
            default:
                no_grouping = 1;
                num_points = data_length;
        }

        if (transform_flags & OUTPUT_LOGARITMIC_TRANSFORM) {
            abs_max = log(abs_max);
            abs_min = log(abs_min);
        }

        *out_ctx = (output_context) {
            .data_buffer_index_to_acc_buffer_index = (unsigned int*) malloc(data_length * sizeof *(out_ctx->data_buffer_index_to_acc_buffer_index)),
                .acc_buffer_data_count = calloc(num_points, sizeof *(out_ctx->acc_buffer_data_count)),
                .acc_buffer_avg_factor = calloc(num_points, sizeof *(out_ctx->acc_buffer_avg_factor)),
                .min_data_index = data_length,
                .max_data_index = 0,
                .abs_min = abs_min,
                .abs_max = abs_max,
                .group_func = group_func,
                .transform_flags = transform_flags,

                .acc_buffer = malloc(num_points * sizeof *(out_ctx->acc_buffer)),
                .smooth_buffer = calloc(num_points, sizeof *(out_ctx->smooth_buffer)),
                .wchar_buffer = malloc((num_points +1) * sizeof *(out_ctx->wchar_buffer)),
                .wchar_silence_buffer = malloc((num_points +1) * sizeof *(out_ctx->wchar_buffer)),
        };


        unsigned int target_acc_index = -1;
        for (i = 0; i < data_length; ++i) {
            double freq = data_frequency[i]; // Frequencies are sorted
            if (freq < min_freq || freq > max_freq) {
                continue;
            }

            out_ctx->min_data_index = min(out_ctx->min_data_index, i);
            out_ctx->max_data_index = max(out_ctx->max_data_index, i);

            if (no_grouping) {
                target_acc_index++;
            } else {
                target_acc_index = ((freq - min_freq) / (max_freq - min_freq)) * num_points;
                target_acc_index = min(target_acc_index, num_points-1); // It's possible that target_acc_index == num_points if freq == max_freq
            }

            out_ctx->data_buffer_index_to_acc_buffer_index[i] = target_acc_index;
            out_ctx->acc_buffer_data_count[target_acc_index]++;
        }

        for (i = 0; i < num_points; ++i) {
            if (out_ctx->acc_buffer_data_count[i] > 0) {
                out_ctx->acc_buffer_avg_factor[i] = 1.0 / out_ctx->acc_buffer_data_count[i];
            } else {
                out_ctx->acc_buffer_avg_factor[i] = 0;
            }
        }

        num_points = target_acc_index + 1; // Overwrite with max available freq index
        out_ctx->num_points = num_points;

        output_set_charset(out_ctx, OUTPUT_CHARSET_BARS);
        output_set_silence_str(out_ctx, NULL);
        output_set_smoothing(out_ctx, OUTPUT_NO_SMOOTH);
        output_set_smoothing_factors(out_ctx, .5, .5);
        output_set_lineal_scale_factor_offset(out_ctx, 0);
    }

    return out_ctx;
}

void output_set_lineal_scale_factor_offset(output_context* out_ctx, double offset) {
    out_ctx->lineal_scaling_factor = 1.0 + offset;
}

void output_set_sigmoid_scale_factor(output_context* out_ctx, double factor) {
    out_ctx->sigmoid_scaling_factor = factor;
}

void output_update_silence_buffer(output_context* out_ctx) {
    unsigned int num_chars = ceil((float) out_ctx->num_points / (float) out_ctx->visualization_points_per_char) + .5, i;

    out_ctx->wchar_silence_buffer[num_chars] = '\0';
    wchar_t* silence_str_source = (out_ctx->provided_silence_str) ? out_ctx->provided_silence_str : silence_str;
    wcsncpy(out_ctx->wchar_silence_buffer, silence_str_source, num_chars);
    for (i = wcslen(silence_str_source); i < num_chars; ++i) {
        out_ctx->wchar_silence_buffer[i] = L' ';
    }
}

void output_set_charset(output_context* out_ctx, int charset) {
    switch (charset) {
        case OUTPUT_CHARSET_BRAILLE:
        case OUTPUT_CHARSET_BRAILLE_WIDE:
            out_ctx->visualization_str = braille_str;
            out_ctx->visualization_levels = braille_levels;
            out_ctx->visualization_points_per_char = braille_points_per_char;

            if (charset == OUTPUT_CHARSET_BRAILLE_WIDE) {
                out_ctx->visualization_points_per_char = 1;
            }

            break;
        case OUTPUT_CHARSET_BARS:
        default:
            out_ctx->visualization_str = bars_str;
            out_ctx->visualization_levels = bars_levels;
            out_ctx->visualization_points_per_char = bars_points_per_char;
    }
    output_update_silence_buffer(out_ctx);
}

void output_set_silence_str(output_context* out_ctx, wchar_t* provided_silence_str) {
    out_ctx->provided_silence_str = provided_silence_str;
    output_update_silence_buffer(out_ctx);
}

void output_set_smoothing(output_context* out_ctx, int smoothing) {
    out_ctx->smoothing = smoothing;
    out_ctx->smoothing_max_limit = 0;
    out_ctx->smoothing_min_limit = 0;
}
void output_set_smoothing_factors(output_context* out_ctx, double new_value_factor, double new_limit_factor) {
    new_value_factor = max(min(new_value_factor, 1.0), 0.0);
    new_limit_factor = max(min(new_limit_factor, 1.0), 0.0);
    out_ctx->smoothing_old_limit_factor = 1 - new_limit_factor;
    out_ctx->smoothing_old_value_factor = 1 - new_value_factor;
    out_ctx->smoothing_new_limit_factor = new_limit_factor;
    out_ctx->smoothing_new_value_factor = new_value_factor;
}


void output_deinit(output_context* out_ctx) {
    free(out_ctx->data_buffer_index_to_acc_buffer_index);
    free(out_ctx->acc_buffer_data_count);
    free(out_ctx->acc_buffer_avg_factor);
    free(out_ctx->acc_buffer);
    free(out_ctx->smooth_buffer);
    free(out_ctx->wchar_buffer);
    free(out_ctx->wchar_silence_buffer);
    free(out_ctx);
}

wchar_t* output_print_silence(output_context* out_ctx) {
    return out_ctx->wchar_silence_buffer;
}

wchar_t* output_print(output_context* out_ctx, double* values) {
    unsigned int i;
    unsigned int num_points = out_ctx->num_points;
    double* acc_buffer = out_ctx->acc_buffer;

    // TRANSFORM
    if (out_ctx->transform_flags & OUTPUT_LOGARITMIC_TRANSFORM) {
        for (i = out_ctx->min_data_index; i <= out_ctx->max_data_index; ++i) {
            values[i] = log(values[i]);
        }
    }

    // ACC
    for (i = 0; i < num_points; ++i) {
        acc_buffer[i] = 0;
    }

    switch (out_ctx->group_func) {
        case OUTPUT_MAX_GROUPING_FUNC:
            for (i = out_ctx->min_data_index; i <= out_ctx->max_data_index; ++i) {
                double value = values[i];
                unsigned int acc_index = out_ctx->data_buffer_index_to_acc_buffer_index[i];

                acc_buffer[acc_index] = max(acc_buffer[acc_index], value);
            }
            break;
        case OUTPUT_AVG_GROUPING_FUNC:
            for (i = out_ctx->min_data_index; i <= out_ctx->max_data_index; ++i) {
                double value = values[i];
                unsigned int acc_index = out_ctx->data_buffer_index_to_acc_buffer_index[i];
                acc_buffer[acc_index] += value;
            }

            for (i = 0; i < num_points; ++i) {
                acc_buffer[i] *= out_ctx->acc_buffer_avg_factor[i];
            }
            break;
        case OUTPUT_NO_GROUPING_FUNC:
        default:
            for (i = out_ctx->min_data_index; i <= out_ctx->max_data_index; ++i) {
                double value = values[i];
                unsigned int acc_index = out_ctx->data_buffer_index_to_acc_buffer_index[i];
                acc_buffer[acc_index] = value;
            }
    }

    // SMOOTH
    double* smooth_buffer = out_ctx->smooth_buffer;
    double min = 0, max = 0;
    switch (out_ctx->smoothing) {
        case OUTPUT_EXP2_SMOOTH:
            {
                double local_min = INFINITY, local_max = 0;
                for (i = 0; i < num_points; ++i) {
                    if (out_ctx->acc_buffer_data_count[i]) {
                        double new_value = max((smooth_buffer[i] * out_ctx->smoothing_old_value_factor + acc_buffer[i] * out_ctx->smoothing_new_value_factor), 0);
                        local_min = min(local_min, new_value);
                        local_max = max(local_max, new_value);
                        smooth_buffer[i] = new_value;
                    } else if (i>0) {
                        smooth_buffer[i] = smooth_buffer[i-1];
                    }
                }
                min = out_ctx->smoothing_min_limit * out_ctx->smoothing_old_limit_factor + local_min * out_ctx->smoothing_new_limit_factor;
                max = out_ctx->smoothing_max_limit * out_ctx->smoothing_old_limit_factor + local_max * out_ctx->smoothing_new_limit_factor;

                out_ctx->smoothing_min_limit = min;
                out_ctx->smoothing_max_limit = max;
            }
            break;
        case OUTPUT_NO_SMOOTH:
        default:
            min = out_ctx->abs_min;
            max = out_ctx->abs_max;
            smooth_buffer = acc_buffer;
    }

    // SCALE & PRINT TO BUFFER
    unsigned int levels = out_ctx->visualization_levels;
    unsigned int points_per_char = out_ctx->visualization_points_per_char;
    wchar_t* symbols = out_ctx->visualization_str;

    unsigned int wchar_index = -1;
    wchar_t* wchar_buffer = out_ctx->wchar_buffer;
    for (i = 0; i < num_points; i+=points_per_char) {
        unsigned int current_point = 0;
        unsigned int current_symbol_index = 0;
        for (current_point = 0; current_point < points_per_char; ++current_point) {
            double level = 0;
            if (i + current_point < num_points) {
                level = ((smooth_buffer[i+current_point] - min) / (max - min)); // Range [0,1]
            }

            if (out_ctx->sigmoid_scaling_factor > 0) {
                level = 1/(1+exp(-out_ctx->sigmoid_scaling_factor * (level-0.5)));
            }

            int f_ranged = (level * out_ctx->lineal_scaling_factor) * levels;
            f_ranged = max(f_ranged, 0);
            f_ranged = min(f_ranged, levels -1);
            current_symbol_index *= levels;
            current_symbol_index += f_ranged;
        }
        wchar_buffer[++wchar_index] = symbols[current_symbol_index]; // wchar_index != i for multipoint chars
    }
    wchar_buffer[wchar_index] = '\0';
    return wchar_buffer;
}

