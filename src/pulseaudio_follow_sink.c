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

#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define UNUSED(x) (void)(x)

typedef struct state_t {
    char monitor_source_name[256];
    uint32_t running_index;
    uint32_t found;
    uint32_t pa_context_ready;
    uint32_t current_stream_source_index;
    pa_mainloop_api* pa_mainloop_api;
    pa_stream* stream;
    const pa_sample_spec* sample_spec;
    const pa_buffer_attr* buffer_attr;

    // Output information, to use in stream callbacks
    unsigned int write_index;
    unsigned int buffer_silence;
    unsigned int n_samples;
    double* output_buffer;
    int (*output_cb)(int, void*);
    void* output_userdata;

    // Need to store that a flush is undergoing
    unsigned int flush_in_progress;
} state_t;

void reset_state(state_t* state) {
    state->monitor_source_name[0] = '\0';
    state->monitor_source_name[255] = '\0';
    state->running_index = 0;
    state->found = 0;
    state->pa_context_ready = 0;
    state->current_stream_source_index = PA_INVALID_INDEX;
    state->pa_mainloop_api = NULL;
    state->stream = NULL;
    state->sample_spec = NULL;
    state->buffer_attr = NULL;

    state->write_index = 0;
    state->buffer_silence = 1;
    state->n_samples = 0;
    state->output_buffer = NULL;
    state->output_cb = NULL;
    state->output_userdata = NULL;

    state->flush_in_progress = 0;
}

static state_t* get_state_from_userdata(void* userdata) {
    return (state_t*) userdata;
}

static void quit(state_t* state_p, int ret_value) {
    state_p->pa_mainloop_api->quit(state_p->pa_mainloop_api, ret_value);
}

// This is for the stream //
static void pa_stream_flush_cb(pa_stream* s, int success, void *userdata) {
    UNUSED(s);
    UNUSED(success);
    get_state_from_userdata(userdata)->flush_in_progress = 0;
}
static void pa_stream_state_cb(pa_stream* s, void* userdata) {
    state_t* state_p = get_state_from_userdata(userdata);
    switch (pa_stream_get_state(s)) {
        case PA_STREAM_FAILED:
        case PA_STREAM_TERMINATED:
            if (state_p->stream == s) {
                pa_stream_disconnect(state_p->stream);
                pa_stream_unref(state_p->stream);
                state_p->stream = NULL;
            }
        default:
            break;
    }
}
static void pa_stream_read_cb(pa_stream* s, size_t length, void* userdata) {
    state_t* state_p = get_state_from_userdata(userdata);
    if (length > 0) {
        const void* data;
        assert(s);

        if (pa_stream_peek(s, &data, &length) < 0) {
            fprintf(stderr, "PA: Could not read from stream: %s\n", pa_strerror(pa_context_errno(pa_stream_get_context(s))));
            state_p->found = 0;
            state_p->current_stream_source_index = PA_INVALID_INDEX;
            return;
        }

        if (data && !state_p->flush_in_progress) {
            if (((uint64_t) data & 1U) == 0) {
                int16_t* pa_buffer = (int16_t*) data;
                length /= 2;
                double* amplitude_samples = state_p->output_buffer;
                unsigned int n_samples = state_p->n_samples;
                int i = 0;

                while (length) {
                    unsigned int limit = n_samples;
                    if (limit > state_p->write_index + length) {
                        limit = state_p->write_index + length;
                        length = 0;
                    } else {
                        length -= (n_samples - state_p->write_index);
                    }

                    for (; state_p->write_index < limit; ++i, ++state_p->write_index) {
                        amplitude_samples[state_p->write_index] = pa_buffer[i];
                        state_p->buffer_silence = state_p->buffer_silence && !pa_buffer[i];
                    }

                    if (state_p->write_index == n_samples) {
                        unsigned int wait_time_ms = state_p->output_cb(state_p->buffer_silence, state_p->output_userdata);
                        state_p->write_index = 0;
                        state_p->buffer_silence = 1;
                        if (wait_time_ms) {
                            sleep(wait_time_ms / 1000); //FIXME Change flush and sleep for cork/uncork
                            state_p->flush_in_progress = 1;
                            pa_stream_flush(s, pa_stream_flush_cb, userdata);
                            break;
                        }
                    } else {
                        break;
                    }
                }
            }
        }

        pa_stream_drop(s);
    }
}
////////////////////////////

static void pa_event_cb(pa_context* c, pa_subscription_event_type_t t, uint32_t sink_index, void* userdata) {
    UNUSED(c);
    if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) == PA_SUBSCRIPTION_EVENT_SINK) {
        state_t* state_p = get_state_from_userdata(userdata);

        if (state_p->found && state_p->running_index == sink_index) {
            state_p->found = 0; // The current running sink has changed, check sinks again
        }

#ifdef DEBUG
        if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_NEW) {
            fprintf(stderr, "PA: Sink new event IDX: %d\n", sink_index);
        } else if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) {
            fprintf(stderr, "PA: Sink remove event IDX: %d\n", sink_index);
        } else {
            fprintf(stderr, "PA: Sink change event IDX: %d\n", sink_index);
        }
#endif
    }
}

static void pa_context_subscribe_completed_cb(pa_context* c, int success, void* userdata) {
    UNUSED(c);
    state_t* state_p = get_state_from_userdata(userdata);
    if (!success) {
        quit(state_p, 0);
    }
}

static void pa_context_state_cb(pa_context* c, void* userdata) {
    state_t* state_p = get_state_from_userdata(userdata);
    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            fprintf(stderr, "PA: Context failed or terminated\n");
            quit(state_p, 0);
            break;
        case PA_CONTEXT_READY:
            pa_context_set_subscribe_callback(c, pa_event_cb, state_p);
            pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK , pa_context_subscribe_completed_cb, state_p);
            state_p->pa_context_ready = 1;
            break;
        default:
            break;
    }
}

static void pa_context_sink_list_cb(pa_context* c, const pa_sink_info* i, int eol, void* userdata) {
    UNUSED(c);
    if (eol <= 0 && i->state == PA_SINK_RUNNING) {
        state_t* state_p = get_state_from_userdata(userdata);
        strncpy(state_p->monitor_source_name, i->monitor_source_name, 256);
        if (state_p->monitor_source_name[255] == '\0') {
            state_p->running_index = i->index;
            state_p->found = 1;
        } else {
            state_p->monitor_source_name[255] = '\0';
            fprintf(stderr, "PA: Monitor source name too big: %s\n", state_p->monitor_source_name);
        }
    }
}

void pa_set_up_read_callback(unsigned int n_samples, unsigned int sample_rate, double* output_buffer, int(*output_cb)(int, void*), void* output_userdata) {
    state_t state;
    state_t* state_p = &state;
    reset_state(state_p);

    state_p->n_samples = n_samples;
    state_p->output_buffer = output_buffer;
    state_p->output_cb = output_cb;
    state_p->output_userdata = output_userdata;

    const pa_sample_spec sample_spec = {
        .format = PA_SAMPLE_S16LE,
        .rate =  sample_rate,
        .channels = 1
    };
    const pa_buffer_attr buffer_attr = {
        .maxlength = sizeof(int16_t) * n_samples,
        .fragsize = -1
    };
    state_p->sample_spec = &sample_spec;
    state_p->buffer_attr = &buffer_attr;

    pa_operation* pa_operation = NULL;
    pa_mainloop* pa_mainloop = pa_mainloop_new();
    state_p->pa_mainloop_api = pa_mainloop_get_api(pa_mainloop);
    pa_context* pa_context = pa_context_new(state_p->pa_mainloop_api, "terminal pulseaudio spectrum");

    pa_context_connect(pa_context, NULL, 0, NULL);

    pa_context_set_state_callback(pa_context, pa_context_state_cb, state_p);
    while (pa_mainloop_iterate(pa_mainloop, 1, NULL) >= 0) {
        if (state_p->pa_context_ready) {
            if (!state_p->found && !pa_operation) {
                pa_operation = pa_context_get_sink_info_list(pa_context, pa_context_sink_list_cb, state_p);
            } else if (pa_operation && pa_operation_get_state(pa_operation) != PA_OPERATION_RUNNING) {
                pa_operation_unref(pa_operation);
                pa_operation = NULL;
            }

            if (state_p->found && state_p->current_stream_source_index != state_p->running_index) {
#ifdef DEBUG
                fprintf(stderr, "PA: Monitor source name for sink %d is \"%s\"\n", state_p->running_index, state_p->monitor_source_name);
#endif

                // This is for the stream //
                if (state_p->stream) {
                    if (pa_stream_disconnect(state_p->stream) < 0) {
                        fprintf(stderr, "PA: Cannot disconnect stream: %s\n", pa_strerror(pa_context_errno(pa_context)));
                        quit(state_p, 0);
                        continue;
                    }
                    pa_stream_unref(state_p->stream);
                    state_p->stream = NULL;
                }

                if (!(state_p->stream = pa_stream_new(pa_context, "terminal pulseaudio spectrum stream", state_p->sample_spec, NULL))) {
                    fprintf(stderr, "PA: Cannot create stream: %s\n", pa_strerror(pa_context_errno(pa_context)));
                    quit(state_p, 0);
                } else {
                    pa_stream_set_state_callback(state_p->stream, pa_stream_state_cb, state_p);
                    pa_stream_set_read_callback(state_p->stream, pa_stream_read_cb, state_p);

                    if (pa_stream_connect_record(state_p->stream, state_p->monitor_source_name, state_p->buffer_attr, 0) < 0) {
                        fprintf(stderr, "PA: Cannot connect to source %s: %s\n", state_p->monitor_source_name, pa_strerror(pa_context_errno(pa_context)));
                    }
                }
                ////////////////////////////

                state_p->current_stream_source_index = state_p->running_index;
            }
        }
    }

    if (state_p->stream) {
        pa_stream_disconnect(state_p->stream);
        pa_stream_unref(state_p->stream);
        state_p->stream = NULL;
    }

    pa_context_disconnect(pa_context);
    pa_context_unref(pa_context);
    pa_mainloop_free(pa_mainloop);
}

