/*
 * SoX Resampler Plugin for Audacious
 * Copyright 2013 Michał Lipski
 *
 * Based on Sample Rate Converter Plugin:
 * Copyright 2010-2020 John Lindgren
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

#include <stdlib.h>
#include <soxr.h>

#include <libfauxdcore/i18n.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/preferences.h>

#define MIN_RATE 8000
#define MAX_RATE 192000
#define RATE_STEP 50

class SoXResampler : public EffectPlugin
{
public:
    static const char about[];
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;

    static constexpr PluginInfo info = {
        N_("SoX Resampler"),
        PACKAGE,
        about,
        & prefs
    };

    /* order #2: must be before crossfade */
    constexpr SoXResampler () : EffectPlugin (info, 2, false) {}

    bool init ();
    void cleanup ();

    void start (int & channels, int & rate);
    Index<audio_sample> & process (Index<audio_sample> & data);
    bool flush (bool force);
};

EXPORT SoXResampler aud_plugin_instance;

const char * const SoXResampler::defaults[] = {
    "quality", aud::numeric_string<SOXR_HQ>::str,
    "rate", "44100",
    "phase_response", aud::numeric_string<SOXR_LINEAR_PHASE>::str,
#ifdef SOXR_ALLOW_ALIASING
    "allow_aliasing", "FALSE",
#endif
    "use_steep_filter", "FALSE",
    nullptr
};

static soxr_t soxr;
static soxr_error_t error;
static soxr_quality_spec_t qspec;
static int stored_rate;
static int target_rate;
static int stored_channels;
static double ratio;
static Index<audio_sample> buffer;

#ifdef DEF_AUDIO_FLOAT64
    static const soxr_io_spec_t iospec = { SOXR_FLOAT64_I, SOXR_FLOAT64_I, 1.0, 0, 0 };
#else
    static const soxr_io_spec_t iospec = { SOXR_FLOAT32_I, SOXR_FLOAT32_I, 1.0, 0, 0 };
#endif

bool SoXResampler::init ()
{
    aud_config_set_defaults ("soxr", defaults);
    return true;
}

void SoXResampler::cleanup ()
{
    soxr_delete (soxr);
    soxr = 0;
    buffer.clear ();
}

void SoXResampler::start (int & channels, int & rate)
{
    soxr_delete (soxr);
    soxr = 0;

    target_rate = aud_get_int ("soxr", "rate");
    target_rate = aud::clamp (target_rate, MIN_RATE, MAX_RATE);

    if (target_rate == rate)
        return;

    stored_rate = rate;

    int recipe = aud_get_int ("soxr", "quality");
    recipe |= aud_get_int ("soxr", "phase_response");
    recipe |= (aud_get_bool ("soxr", "use_steep_filter")) ? SOXR_STEEP_FILTER : 0;
#ifdef SOXR_ALLOW_ALIASING
    recipe |= (aud_get_bool ("soxr", "allow_aliasing")) ? SOXR_ALLOW_ALIASING : 0;
#endif

    qspec = soxr_quality_spec (recipe, 0);

    soxr = soxr_create (rate, target_rate, channels, & error, & iospec, & qspec, nullptr);

    if (error)
    {
        AUDERR ("%s\n", error);
        return;
    }

    stored_channels = channels;
    ratio = (double) target_rate / rate;
    rate = target_rate;
}

Index<audio_sample> & SoXResampler::process (Index<audio_sample> & data)
{
    if (! soxr)
         return data;

    buffer.resize ((int) (data.len () * ratio) + 256);

    size_t samples_done;
    error = soxr_process (soxr, data.begin (), data.len () / stored_channels,
     nullptr, buffer.begin (), buffer.len () / stored_channels, & samples_done);

    if (error)
    {
        AUDERR ("%s\n", error);
        return data;
    }

    buffer.resize (samples_done * stored_channels);

    return buffer;
}

bool SoXResampler::flush (bool force)
{
    if (! soxr)
        return true;

    soxr_delete (soxr);

    soxr = soxr_create (stored_rate, target_rate, stored_channels, & error, & iospec, & qspec, nullptr);

    if (error)
        AUDERR ("%s\n", error);

    return true;
}

const char SoXResampler::about[] =
 N_("SoX Resampler Plugin for Audacious\n"
    "Copyright 2013 Michał Lipski\n\n"
    "Based on Sample Rate Converter Plugin:\n"
    "Copyright 2010-2012 John Lindgren");

static const ComboItem method_list[] = {
    ComboItem (N_("Quick"), SOXR_QQ),
    ComboItem (N_("Low"), SOXR_LQ),
    ComboItem (N_("Medium"), SOXR_MQ),
    ComboItem (N_("High"), SOXR_HQ),
    ComboItem (N_("Very High"), SOXR_VHQ),
    ComboItem (N_("Ultra High"), SOXR_32_BITQ)
};

static const ComboItem phase_response_list[] = {
    ComboItem (N_("Minimum"), SOXR_MINIMUM_PHASE),
    ComboItem (N_("Intermediate"), SOXR_INTERMEDIATE_PHASE),
    ComboItem (N_("Linear"), SOXR_LINEAR_PHASE)
};

const PreferencesWidget SoXResampler::widgets[] = {
    WidgetCombo (N_("Quality:"),
        WidgetInt ("soxr", "quality"),
        {{method_list}}),
    WidgetCombo (N_("Phase:"),
        WidgetInt ("soxr", "phase_response"),
        {{phase_response_list}}),
#ifdef SOXR_ALLOW_ALIASING
    WidgetCheck (N_("Allow aliasing"), WidgetBool ("soxr", "allow_aliasing")),
#endif
    WidgetCheck (N_("Use steep filter"), WidgetBool ("soxr", "use_steep_filter")),
    WidgetSpin (N_("Rate:"),
        WidgetInt ("soxr", "rate"),
        {MIN_RATE, MAX_RATE, RATE_STEP, N_("Hz")})
};

const PluginPreferences SoXResampler::prefs = {{widgets}};
