#ifndef PULSEAUDIO_FOLLOW_SINK_H
#define PULSEAUDIO_FOLLOW_SINK_H

void pa_set_up_read_callback(
        unsigned int n_samples,
        unsigned int sample_rate,
        double* buffer_to_use,
        int(*output_cb)(int, void*),
        void* output_userdata
        );

#endif

