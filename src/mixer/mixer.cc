/*
 * Channel Mixer Plugin for Audacious
 * Copyright 2011-2012 John Lindgren and Michał Lipski
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

/* TODO: implement more surround converters.
         There should be more options for in * out cases (for example,
         the user may wish to mix stereo up to quadro but keep 5.1 as-is,
         rather than downmixing 5.1 to quadro). A possible design might
         be a choice of output channels for each input channel count that
         we care about. */


#include <stdlib.h>

#include <libfauxdcore/i18n.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/preferences.h>

class ChannelMixer : public EffectPlugin
{
public:
    static const char about[];
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("Channel Mixer"),
        PACKAGE,
        about,
        & prefs
    };

    /* order #2: must be before crossfade */
    constexpr ChannelMixer () : EffectPlugin (info, 2, false) {}

    bool init ();
    void cleanup ();

    void start (int & channels, int & rate);
    Index<audio_sample> & process (Index<audio_sample> & data);
};

EXPORT ChannelMixer aud_plugin_instance;

typedef Index<audio_sample> & (* Converter) (Index<audio_sample> & data);

static Index<audio_sample> mixer_buf;

static Index<audio_sample> & mono_to_stereo (Index<audio_sample> & data)
{
    int frames = data.len ();
    mixer_buf.resize (2 * frames);

    audio_sample * get = data.begin ();
    audio_sample * set = mixer_buf.begin ();

    while (frames --)
    {
        audio_sample val = * get ++;
        * set ++ = val;
        * set ++ = val;
    }

    return mixer_buf;
}

static Index<audio_sample> & stereo_to_mono (Index<audio_sample> & data)
{
    int frames = data.len () / 2;
    mixer_buf.resize (frames);

    audio_sample * get = data.begin ();
    audio_sample * set = mixer_buf.begin ();

    while (frames --)
    {
        audio_sample val = * get ++;
        val += * get ++;
        * set ++ = val / 2;
    }

    return mixer_buf;
}

static Index<audio_sample> & quadro_to_stereo (Index<audio_sample> & data)
{
    int frames = data.len () / 4;
    mixer_buf.resize (2 * frames);

    audio_sample * get = data.begin ();
    audio_sample * set = mixer_buf.begin ();

    while (frames --)
    {
        audio_sample front_left  = * get ++;
        audio_sample front_right = * get ++;
        audio_sample back_left   = * get ++;
        audio_sample back_right  = * get ++;
        * set ++ = front_left + (back_left * 0.7);
        * set ++ = front_right + (back_right * 0.7);
    }

    return mixer_buf;
}

static Index<audio_sample> & stereo_to_quadro (Index<audio_sample> & data)
{
    int frames = data.len () / 2;
    mixer_buf.resize (4 * frames);

    audio_sample * get = data.begin ();
    audio_sample * set = mixer_buf.begin();

    while (frames --)
    {
        audio_sample left  = * get ++;
        audio_sample right = * get ++;
        * set ++ = left;   // front left
        * set ++ = right;  // front right
        * set ++ = left;   // rear left
        * set ++ = right;  // rear right
    }

    return mixer_buf;
}

static Index<audio_sample> & surround_5p1_to_stereo (Index<audio_sample> & data)
{
    int frames = data.len () / 6;
    mixer_buf.resize (2 * frames);

    audio_sample * get = data.begin ();
    audio_sample * set = mixer_buf.begin ();

    while (frames --)
    {
        audio_sample front_left  = * get ++;
        audio_sample front_right = * get ++;
        audio_sample center = * get ++;
        audio_sample lfe    = * get ++;
        audio_sample rear_left   = * get ++;
        audio_sample rear_right  = * get ++;
        * set ++ = front_left + (center * 0.5) + (lfe * 0.5) + (rear_left * 0.5);
        * set ++ = front_right + (center * 0.5) + (lfe * 0.5) + (rear_right * 0.5);
    }

    return mixer_buf;
}

/* 5 channels case. Quad + center channel */
static Index<audio_sample> & quadro_5_to_stereo (Index<audio_sample> & data)
{
    int frames = data.len () / 5;
    mixer_buf.resize (2 * frames);

    audio_sample * get = data.begin ();
    audio_sample * set = mixer_buf.begin ();

    while (frames --)
    {
        audio_sample front_left  = * get ++;
        audio_sample front_right = * get ++;
        audio_sample center = * get ++;
        audio_sample rear_left   = * get ++;
        audio_sample rear_right  = * get ++;
        * set ++ = front_left + (center * 0.5) + rear_left;
        * set ++ = front_right + (center * 0.5) + rear_right;
    }

    return mixer_buf;
}

static Converter get_converter (int in, int out)
{
    if (in == 1 && out == 2)
        return mono_to_stereo;
    if (in == 2 && out == 1)
        return stereo_to_mono;
    if (in == 2 && out == 4)
        return stereo_to_quadro;
    if (in == 4 && out == 2)
        return quadro_to_stereo;
    if (in == 5 && out == 2)
        return quadro_5_to_stereo;
    if (in == 6 && out == 2)
        return surround_5p1_to_stereo;

    return nullptr;
}

static int input_channels, output_channels;

void ChannelMixer::start (int & channels, int & rate)
{
    input_channels = channels;
    output_channels = aud_get_int ("mixer", "channels");

    if (input_channels == output_channels)
        return;

    if (! get_converter (input_channels, output_channels))
    {
        AUDERR ("Converting %d to %d channels is not implemented.\n",
         input_channels, output_channels);
        return;
    }

    channels = output_channels;
}

Index<audio_sample> & ChannelMixer::process (Index<audio_sample> & data)
{
    if (input_channels == output_channels)
        return data;

    Converter converter = get_converter (input_channels, output_channels);
    if (converter)
        return converter (data);

    return data;
}

const char * const ChannelMixer::defaults[] = {
 "channels", "2",
  nullptr};

bool ChannelMixer::init ()
{
    aud_config_set_defaults ("mixer", defaults);
    return true;
}

void ChannelMixer::cleanup ()
{
    mixer_buf.clear ();
}

const char ChannelMixer::about[] =
 N_("Channel Mixer Plugin for Audacious\n"
    "Copyright 2011-2012 John Lindgren and Michał Lipski");

const PreferencesWidget ChannelMixer::widgets[] = {
    WidgetLabel (N_("<b>Channel Mixer</b>")),
    WidgetSpin (N_("Output channels:"),
        WidgetInt ("mixer", "channels"),
        {1, AUD_MAX_CHANNELS, 1})
};

const PluginPreferences ChannelMixer::prefs = {{widgets}};
