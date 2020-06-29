/*
 * albumart.c
 * Copyright 2012-2013 John Lindgren
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

#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

#ifdef _WIN32
#include <winbase.h>
#endif

#include <libfauxdcore/preferences.h>
#include <libfauxdcore/drct.h>
#include <libfauxdcore/i18n.h>
#include <libfauxdcore/plugin.h>
#include <libfauxdcore/hook.h>
#include <libfauxdcore/audstrings.h>
#include <libfauxdcore/runtime.h>
#include <libfauxdgui/libfauxdgui.h>
#include <libfauxdgui/libfauxdgui-gtk.h>

class AlbumArtPlugin : public GeneralPlugin
{
public:
    static const char * const defaults[];
    static const PreferencesWidget widgets[];
    static const PluginPreferences prefs;
    static constexpr PluginInfo info = {
        N_("Album Art"),
        PACKAGE,
        nullptr, // about
        & prefs,
        PluginGLibOnly
    };

    constexpr AlbumArtPlugin () : GeneralPlugin (info, false) {}

    bool init ();
    void * get_gtk_widget ();
};

EXPORT AlbumArtPlugin aud_plugin_instance;

const char * const AlbumArtPlugin::defaults[] = {
    "internet_coverartlookup", "FALSE",
    nullptr
};

bool AlbumArtPlugin::init ()
{
    aud_config_set_defaults ("albumart", defaults);
    return true;
}

static bool frominit = false;  // TRUE WHEN THREAD STARTED BY SONG CHANGE (album_init()).
static bool skipreset = false; // TRUE WHILE THREAD RUNNING AFTER STARTED BY SONG CHANGE (album_init()).
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/* JWT:SEPARATE THREAD TO CALL THE HELPER SO THAT THE "LONG" TIME IT TAKES DOESN'T FREEZE THE GUI
   DISPLAY WHILE ATTEMPTING TO FIND AND FETCH THE ALBUM-ART.  THIS THREAD MUST *NOT* CALL THE
   ART FUNCTIONS THOUGH - CAUSES GUI ISSUES!  WHEN STARTING A NEW SONG/STREAM, WE WAIT FOR 2
   SECONDS BEFORE FETCHING IMAGE TO ALLOW THE TUPLE TO CHANGE (IE. RESTARTING A STREAMING STATION
   LATER USUALLY MEANS A DIFFERENT SONG TITLE), OTHERWISE, WE'D CALL THE THREAD TWICE, ONE FOR THE
   PREV. SONG TITLE STILL DISPLAYED, THEN AGAIN WHEN THE TUPLE CHANGES (USUALLY, ALMOST IMMEDIATELY)!
*/
static void * helper_thread_fn (void * data)
{
    if (frominit)
    {
        skipreset = true;
        g_usleep (2000000);  // SLEEP 2" TO ALLOW FOR ANY TUPLE CHANGE TO OVERRIDE! */
        if (! frominit)
        {
            skipreset = false;
            pthread_exit (nullptr);
            return nullptr;
        }
    }
    pthread_mutex_lock (& mutex);

    String cover_helper = aud_get_str ("audacious", "cover_helper");

    if (cover_helper && cover_helper[0]) //JWT:WE HAVE A PERL HELPER TO LOOK UP COVER ART.
    {
        Tuple tuple = aud_drct_get_tuple ();
        String Title = tuple.get_str (Tuple::Title);
        String Artist = tuple.get_str (Tuple::Artist);
        String Album = tuple.get_str (Tuple::Album);
        const char * album = (const char *) Album;
        if (Title && Title[0])
        {
            if (album && ! strstr (album, "://"))  // ALBUM FIELD NOT BLANK AND NOT A FILE/URL:
            {
                if (aud_get_bool (nullptr, "split_titles"))
                {
                    /* ALBUM MAY ALSO CONTAIN THE STREAM NAME (IE. "<ALBUM> - <STREAM NAME>"): STRIP THAT OFF: */
                    const char * throwaway = strstr (album, " - ");
                    int albumlen = throwaway ? throwaway - album : -1;
                    Album = String (str_copy (album, albumlen));
                }
            }
            else
                Album = String ("_");

            if (! aud_get_bool (nullptr, "split_titles"))
            {
                /* ARTIST MAY BE IN TITLE INSTEAD (IE. "<ARTIST> - <TITLE>"): IF SO, USE THAT FOR ARTIST: */
                const char * title = (const char *) Title;
                if (title)
                {
                    const char * artistlen = strstr (title, " - ");
                    if (artistlen)
                    {
                        Artist = String (str_copy (title, artistlen - title));
                        const char * titleoffset = artistlen+3;
                        if (titleoffset)
                            Title = String (str_copy (artistlen+3, -1));
                    }
                }
            }
            if (!Artist || !Artist[0])
                Artist = String ("_");
            StringBuf album_buf = str_encode_percent (Album);
            StringBuf artist_buf = str_encode_percent (Artist);
            StringBuf title_buf = str_encode_percent (Title);

#ifdef _WIN32
            WinExec ((const char *) str_concat ({cover_helper, " ALBUM '",
                    (const char *) album_buf, "' ", aud_get_path (AudPath::UserDir), " '",
                    (const char *) artist_buf, "' '", (const char *) title_buf, "' "}),
                    SW_HIDE);
#else
            system ((const char *) str_concat ({cover_helper, " ALBUM '",
                    (const char *) album_buf, "' ", aud_get_path (AudPath::UserDir), " '",
                    (const char *) artist_buf, "' '", (const char *) title_buf, "' "}));
#endif

            hook_call ("albumart ready", nullptr);
        }
    }

    cover_helper = String ();

    pthread_mutex_unlock (& mutex);

    skipreset = false;
    pthread_exit (nullptr);
    return nullptr;
}

/* JWT:CALLED BY ALBUMART FETCHING THREAD WHEN IT HAS FINISHED SEARCHING/FETCHING ALBUM-COVER IMAGE */
static void albumart_ready (void *, GtkWidget * widget)
{
    AudguiPixbuf pixbuf;
    String cover_helper = aud_get_str ("audacious", "cover_helper");
    String coverart_file;
    Index<String> extlist = str_list_to_index ("jpg,png,jpeg,gif", ",");

    for (auto & ext : extlist)
    {
        coverart_file = String (str_concat ({"file://", aud_get_path (AudPath::UserDir), "/_tmp_albumart.", (const char *) ext}));
        const char * filenamechar = coverart_file + 7;
        struct stat statbuf;
        if (stat (filenamechar, &statbuf) >= 0)  // ART IMAGE FILE EXISTS:
        {
            coverart_file = String (filename_to_uri (filenamechar));
            pixbuf = audgui_pixbuf_request ((const char *) coverart_file);
            if (pixbuf)
                audgui_scaled_image_set (widget, pixbuf.get ());

            return;
        }
    }
    return;
}

/* JWT:UPDATE THE ALBUM-COVER IMAGE (CALL THREAD IF DYNAMIC ALBUM-ART OPTION IN EFFECT): */
static void album_update (void *, GtkWidget * widget)
{
    bool haveartalready = false;

    if (! skipreset)
    {
        AudguiPixbuf pixbuf = audgui_pixbuf_request_current ();

        if (pixbuf)
            haveartalready = true;
        else
            pixbuf = audgui_pixbuf_fallback ();

        if (pixbuf)
            audgui_scaled_image_set (widget, pixbuf.get ());
        else
            haveartalready = false;
    }

    if (aud_get_str ("audacious", "cover_helper") && aud_get_bool ("albumart", "internet_coverartlookup"))
    {
        if (haveartalready)  /* JWT:IF SONG IS A FILE & ALREADY HAVE ART IMAGE, SKIP INTERNET ART SEARCH! */
        {
            String filename = aud_drct_get_filename ();
            if (! strncmp (filename, "file://", 7))
                return;
        }

        pthread_attr_t thread_attrs;
        if (! pthread_attr_init (& thread_attrs))
        {
            if (! pthread_attr_setdetachstate (& thread_attrs, PTHREAD_CREATE_DETACHED)
                    || ! pthread_attr_setscope (& thread_attrs, PTHREAD_SCOPE_PROCESS))
            {
                pthread_t helper_thread;

                if (pthread_create (&helper_thread, nullptr, helper_thread_fn, widget))
                    AUDERR ("s:Error creating helper thread: %s - Expect Delays!...\n", strerror (errno));
            }
            else
                AUDERR ("s:Error detatching helper thread: %s!\n", strerror (errno));

            if (pthread_attr_destroy (& thread_attrs))
                AUDERR ("s:Error destroying helper thread attributes: %s!\n", strerror (errno));
        }
        else
            AUDERR ("s:Error initializing helper thread attributes: %s!\n", strerror (errno));
    }
}

/* JWT:CALLED WHEN SONG ENTRY CHANGES: */
static void album_init (void *, GtkWidget * widget)
{
    if (aud_get_bool ("albumart", "internet_coverartlookup"))
    {
        frominit = true;
        album_update (nullptr, widget);  // JWT:CHECK FILES & DISKS (TUPLE DOESN'T CHANGE IN THESE) ONCE NOW ON PLAY START!
    }
    else
    {
        AudguiPixbuf pixbuf = audgui_pixbuf_request_current ();

        if (! pixbuf)
            pixbuf = audgui_pixbuf_fallback ();

        if (pixbuf)
            audgui_scaled_image_set (widget, pixbuf.get ());
    }
}

/* JWT:CALLED WHEN TITLE CHANGES WITHIN THE SAME SONG/STREAM ENTRY: */
static void album_tuplechg (void *, GtkWidget * widget)
{
    frominit = false;
    album_update (nullptr, widget);
}

/* JWT:CALLED WHEN PLAY IS STOPPED (BUT NOT WHEN JUMPING BETWEEN ENTRIES: */
static void album_clear (void *, GtkWidget * widget)
{
    audgui_scaled_image_set (widget, nullptr);
}

static void album_cleanup (GtkWidget * widget)
{
    hook_dissociate ("playback stop", (HookFunction) album_clear, widget);
    hook_dissociate ("albumart ready", (HookFunction) albumart_ready, widget);
    hook_dissociate ("tuple change", (HookFunction) album_tuplechg, widget);
    hook_dissociate ("playback ready", (HookFunction) album_init, widget);

    audgui_cleanup ();
}

void * AlbumArtPlugin::get_gtk_widget ()
{
    audgui_init ();

    GtkWidget * widget = audgui_scaled_image_new (nullptr);

    g_signal_connect (widget, "destroy", (GCallback) album_cleanup, nullptr);

    hook_associate ("playback ready", (HookFunction) album_init, widget);
    hook_associate ("tuple change", (HookFunction) album_tuplechg, widget);
    hook_associate ("albumart ready", (HookFunction) albumart_ready, widget);
    hook_associate ("playback stop", (HookFunction) album_clear, widget);

    if (aud_drct_get_ready ())
        album_init (nullptr, widget);

    return widget;
}

/* JWT:FIXME: THIS IS MARKED "EXPERIMENTAL" IN WINDOWS SINCE GUI-INTERACTION CAN
    BECOME INVISIBLE AFTER A TIME UNTIL PLAY STOPPED & RESTARTED LEADING TO A BAD
    USER-EXPERIENCE, AND I HAVEN'T BEEN ABLE TO FIGURE OUT WHY?!?!?!
*/

const PreferencesWidget AlbumArtPlugin::widgets[] = {
    WidgetLabel(N_("<b>Albumart Configuration</b>")),
#ifdef _WIN32
    WidgetCheck (N_("Look for album art on Musicbrainz.com (EXPERIMENTAL!)"),
#else
    WidgetCheck (N_("Look for album art on Musicbrainz.com"),
#endif
        WidgetBool ("albumart", "internet_coverartlookup")),
};

const PluginPreferences AlbumArtPlugin::prefs = {{widgets}};
