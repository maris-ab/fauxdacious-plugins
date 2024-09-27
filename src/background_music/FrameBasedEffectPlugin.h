#ifndef AUDACIOUS_PLUGINS_BGM_FRAMEBASEDEFFECTPLUGIN_H
#define AUDACIOUS_PLUGINS_BGM_FRAMEBASEDEFFECTPLUGIN_H
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
#include "LoudnessFrameProcessor.h"
#include <libfauxdcore/plugin.h>

class FrameBasedEffectPlugin : public EffectPlugin
{
    Index<audio_sample> frame_in;
    Index<audio_sample> frame_out;
    Index<audio_sample> output;
    int current_channels = 0, current_rate = 0, channel_last_read = 0;
    LoudnessFrameProcessor detection;

public:
    FrameBasedEffectPlugin(const PluginInfo & info, int order)
        : EffectPlugin(info, order, true)
    {
    }

    virtual ~FrameBasedEffectPlugin() = default;

    bool init() final
    {
        detection.init();
        return true;
    }

    void cleanup() final
    {
        output.clear();
        frame_in.clear();
        frame_out.clear();
    }

    void start(int & channels, int & rate) final
    {
        current_channels = channels;
        current_rate = rate;
        channel_last_read = 0;

        detection.start(channels, rate);
        frame_in.resize(current_channels);
        frame_out.resize(current_channels);

        flush(false);
    }

    Index<audio_sample> & process(Index<audio_sample> & data) final
    {
        detection.update_config();

        int output_samples = 0;
        output.resize(0);

        // It is assumed data always contains a multiple of channels, but we
        // don't care.
        for (const audio_sample sample : data)
        {
            frame_in[channel_last_read++] = sample;
            if (channel_last_read == current_channels)
            {
                // Processing happens per frame. Because of read-ahead there is
                // not always output available yet.
                if (detection.process_has_output(frame_in, frame_out))
                {
                    output.move_from(frame_out, 0, output_samples,
                                     current_channels, true, false);
                    output_samples += current_channels;
                }
                channel_last_read = 0;
            }
        }

        return output;
    }

    bool flush(bool force) final
    {
        detection.flush();
        return true;
    }

    Index<audio_sample> & finish(Index<audio_sample> & data, bool end_of_playlist) final
    {
        return process(data);
    }

    int adjust_delay(int delay) final
    {
        auto result =
            aud::rescale<int64_t>(detection.latency(), current_rate, 1000);
        result += delay;
        return static_cast<int>(result);
    }
};

#endif // AUDACIOUS_PLUGINS_BGM_FRAMEBASEDEFFECTPLUGIN_H
