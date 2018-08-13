#ifndef WCHAR_OUTPUT_H
#define WCHAR_OUTPUT_H

#include <wchar.h>

#define OUTPUT_NO_GROUPING         0U
#define OUTPUT_LINEAL_GROUPING     1U
#define OUTPUT_LOGARITMIC_GROUPING 2U

#define OUTPUT_NO_GROUPING_FUNC  0U
#define OUTPUT_MAX_GROUPING_FUNC 1U
#define OUTPUT_AVG_GROUPING_FUNC 2U

#define OUTPUT_NO_TRANSFORM         0U
#define OUTPUT_LOGARITMIC_TRANSFORM 1U

typedef struct output_context output_context;

output_context* output_init(
        unsigned int data_length,
        double* data_frequency,
        unsigned int min_freq,
        unsigned int max_freq,
        unsigned int num_points,
        double abs_min,
        double abs_max,
        int group,               // No grouping/Lineal
        int group_func,          // MAX/AVG
        unsigned int transform_flags
        );

void output_set_lineal_scale_factor_offset(output_context* out_ctx, double offset);

void output_set_sigmoid_scale_factor(output_context* out_ctx, double factor);

#define OUTPUT_CHARSET_BARS         1U
#define OUTPUT_CHARSET_BRAILLE      2U
#define OUTPUT_CHARSET_BRAILLE_WIDE 3U
void output_set_charset(output_context* out_ctx, int charset);

void output_set_silence_str(output_context* out_ctx, wchar_t* provided_silence_str);

#define OUTPUT_NO_SMOOTH   0U
#define OUTPUT_EXP2_SMOOTH 1U
void output_set_smoothing(output_context* out_ctx, int smoothing);
void output_set_smoothing_factors(output_context* out_ctx, double new_value_factor, double new_limit_factor);

void output_deinit(output_context* out_ctx);

wchar_t* output_print_silence(output_context* out_ctx);

wchar_t* output_print(output_context* out_ctx, double* values);

#endif

