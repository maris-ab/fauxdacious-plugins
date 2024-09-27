#ifndef AUDACIOUS_PLUGINS_BGM_LOUDNESS_FRAME_PROCESSOR_H
#define AUDACIOUS_PLUGINS_BGM_LOUDNESS_FRAME_PROCESSOR_H
/*
 * Background music (equal loudness) Plugin for Audacious
 * Copyright 2023 Michel Fleur
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */
#include "Integrator.h"
#include "Loudness.h"
#include "basic_config.h"
#include <cmath>
#include <libfauxdcore/runtime.h>

class LoudnessFrameProcessor
{
    static constexpr float SHORT_INTEGRATION = 0.4;
    static constexpr float LONG_INTEGRATION = 6.3;
    /*
     * This adjusts the slow RMS measurement so that it's displayed correctly
     * in the audacious VU meter. This only helps for expectation management.
     */
    static constexpr float SLOW_VU_FUDGE_FACTOR = 2.0f;
    static constexpr float FAST_VU_FUDGE_FACTOR = 3.0f;

    FastAttackSmoothRelease release_integration;
    Integrator long_integration;
    PerceptiveRMS perceivedLoudness;
    float slow_weight = 0;
    float target_level = 0.1;
    float maximum_amplification = 1;
    float perception_slow_balance = 0.3;
    audio_sample minimum_detection = 1e-6;
    RingBuf<audio_sample> read_ahead_buffer;
    int channels_ = 0;
    int processed_frames = 0;

    static float get_clamped_value(const char * variable, const double minimum,
                                   const double maximum)
    {
        return static_cast<float>(aud::clamp(
            aud_get_double(CONFIG_SECTION_BACKGROUND_MUSIC, variable),
            minimum, maximum));
    }

    static float get_clamped_decibel_value(const char * variable,
                                           const double minimum,
                                           const double maximum)
    {
        const float decibels = get_clamped_value(variable, minimum, maximum);
        return powf(10.0f, 0.05f * decibels);
    }

public:
    [[nodiscard]] int latency() const { return perceivedLoudness.latency(); }

    LoudnessFrameProcessor()
    {
        aud_config_set_defaults(CONFIG_SECTION_BACKGROUND_MUSIC,
                                background_music_defaults);
    }

    void init()
    {
        update_config();
        long_integration.set_output(0);
        release_integration.set_output(target_level * target_level);
        minimum_detection = target_level / maximum_amplification;
    }

    void start(const int channels, int rate)
    {
        update_config();
        channels_ = channels;
        processed_frames = 0;
        release_integration.set_seconds_for_rate(SHORT_INTEGRATION, rate, 0);
        long_integration.set_seconds_for_rate(LONG_INTEGRATION / 2.0, rate,
                                              slow_weight);
        /*
         * This RMS (Root-mean-square) calculation integrates squared samples
         * with the RC-style integrator and then draws the square root. This has
         * the effect that rises in averages are tracked twice as fast while
         * decreases are tracked twice as slow. As the decrease is what "counts"
         * for the effective time the signal climbs back up after a peak, we
         * must therefore half the integration time.
         */
        perceivedLoudness.set_rate_and_value(rate, target_level);
        const int alloc_size = channels_ * latency();

        if (read_ahead_buffer.size() < alloc_size)
        {
            read_ahead_buffer.alloc(alloc_size);
        }
    }

    void update_config()
    {
        target_level = get_clamped_decibel_value(CONF_TARGET_LEVEL_VARIABLE,
                                                 CONF_TARGET_LEVEL_MIN,
                                                 CONF_TARGET_LEVEL_MAX);
        maximum_amplification = get_clamped_decibel_value(
            CONF_MAX_AMPLIFICATION_VARIABLE, CONF_MAX_AMPLIFICATION_MIN,
            CONF_MAX_AMPLIFICATION_MAX);
        perception_slow_balance = get_clamped_value(
            CONF_SLOW_WEIGHT_VARIABLE, CONF_SLOW_WEIGHT_MIN,
                              CONF_SLOW_WEIGHT_MAX);
        minimum_detection = target_level / maximum_amplification;
        slow_weight = 2.0f * perception_slow_balance * SLOW_VU_FUDGE_FACTOR;
        slow_weight *= slow_weight;
        long_integration.set_scale(slow_weight);
    }

    bool process_has_output(const Index<audio_sample> & frame_in,
                            Index<audio_sample> & frame_out)
    {
        bool has_output_data = processed_frames >= latency();
        if (has_output_data)
        {
            read_ahead_buffer.move_out(frame_out.begin(), channels_);
        }
        else
        {
            processed_frames++;
        }
        read_ahead_buffer.copy_in(frame_in.begin(), channels_);

        /*
         * Following calculations need to happen to anticipate the (future)
         * output.
         */

        audio_sample square_sum = 0.0;
        audio_sample square_max = 0.0;
        for (const audio_sample sample : frame_in)
        {
            const audio_sample square = sample * sample;
            square_max = std::max(square_max, square);
            square_sum += square;
        }
        square_sum /= static_cast<audio_sample>(channels_);
        square_sum += square_max;
        const audio_sample perceived = FAST_VU_FUDGE_FACTOR *
                                perceivedLoudness.get_mean_squared(square_sum);
        const double weighted =
            std::max(long_integration.integrate(square_sum), perceived);

        const double rms = sqrt(weighted);

        const audio_sample gain =
            target_level /
            std::max(minimum_detection,
                     static_cast<audio_sample>(release_integration.get_envelope(rms)));

        if (has_output_data)
        {
            for (audio_sample & sample : frame_out)
            {
                sample *= gain;
            }
        }

        return has_output_data;
    }

    void flush()
    {
        processed_frames = 0;
        read_ahead_buffer.discard();
    }
};

#endif // AUDACIOUS_PLUGINS_BGM_LOUDNESS_FRAME_PROCESSOR_H
