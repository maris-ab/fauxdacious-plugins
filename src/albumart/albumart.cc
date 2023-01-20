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
#include <utime.h>
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

static bool fromsongstartup = false;  // TRUE WHEN THREAD STARTED BY SONG CHANGE (album_init()).
static bool skipArtReInit = false;    // JWT:TRUE:SKIP RESETTING ART (ALREADY RESET BY THREAD NOW SLEEPING).
static bool resetthreads = false;     // JWT:TRUE STOP ANY THREADS RUNNING ON SONG CHANGE OR SHUTDOWN.
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static bool hide_dup_art_icon;   /* JWT:TOGGLE TO TRUE TO HIDE (DUPLICATE) ART ICON IN INFOBAR IF A WEB IMAGE FETCHED. */
static bool last_image_from_web; /* JWT:TRUE IF LAST IMAGE CAME FROM WEB ("Look for album art on the web" OPTION). */

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
    "scale_to_fill", "FALSE",
    "save_by_songfile", "FALSE",
    nullptr
};

bool AlbumArtPlugin::init ()
{
    aud_config_set_defaults ("albumart", defaults);
    hide_dup_art_icon = aud_get_bool ("albumart", "hide_dup_art_icon");
    return true;
}

/* CALLED BY g_idle_add() TO UPDATE ALBUMART WIDGET FROM THREAD: */
static gboolean albumart_ready (gpointer widget)
{
    AudguiPixbuf pixbuf;
    String coverart_file;
    Index<String> extlist = str_list_to_index ("jpg,png,gif,jpeg,webp", ",");

    for (auto & ext : extlist)
    {
        coverart_file = String (str_concat ({aud_get_path (AudPath::UserDir), "/_tmp_albumart.", (const char *) ext}));
        const char * filenamechar = coverart_file;
        struct stat statbuf;
        if (stat (filenamechar, &statbuf) >= 0)  // ART IMAGE FILE FROM WEB EXISTS (THUS NOT A DUP.):
        {
            coverart_file = String (filename_to_uri (filenamechar));
            pixbuf = audgui_pixbuf_request ((const char *) coverart_file);
            if (pixbuf)
            {
                audgui_scaled_image_set ((GtkWidget *) widget, pixbuf.get ());
                /* INFOBAR ICON POSSIBLY HIDDEN BY HIDE DUP. OPTION, SO FORCE "SHOW" IN INFOBAR: */
                aud_set_int ("albumart", "_infoarea_hide_art_gtk", 0);
                hook_call ("gtkui toggle infoarea_art", nullptr);
                last_image_from_web = true;
            }
            return false;
        }
    }
    return false;
}

/* JWT:SEPARATE THREAD TO CALL THE HELPER SO THAT THE "LONG" TIME IT TAKES DOESN'T FREEZE THE GUI
   DISPLAY WHILE ATTEMPTING TO FIND AND FETCH THE ALBUM-ART.  THIS THREAD MUST *NOT* CALL THE
   ART FUNCTIONS THOUGH - CAUSES GUI ISSUES!  WHEN STARTING A NEW SONG/STREAM, WE WAIT FOR 2
   SECONDS BEFORE FETCHING IMAGE TO ALLOW THE TUPLE TO CHANGE (IE. RESTARTING A STREAMING STATION
   LATER USUALLY MEANS A DIFFERENT SONG TITLE), OTHERWISE, WE'D CALL THE THREAD TWICE, ONE FOR THE
   PREV. SONG TITLE STILL DISPLAYED, THEN AGAIN WHEN THE TUPLE CHANGES (USUALLY, ALMOST IMMEDIATELY)!
*/
static void * album_helper_thread_fn (void * data)
{
    bool abortthisthread = resetthreads;
    if (abortthisthread)
    {
        pthread_exit (nullptr);
        return nullptr;
    }
    if (fromsongstartup)
    {
        String filename = aud_drct_get_filename ();
        if (! strcmp_nocase (filename, "https://", 8) || ! strcmp_nocase (filename, "http://", 7))
        {
            int sleep_msec = aud_get_int ("albumart", "sleep_msec");
            if (sleep_msec < 1)  sleep_msec = 1500;
            skipArtReInit = true;
            g_usleep (sleep_msec * 1000);  // SLEEP 2" TO ALLOW FOR ANY TUPLE CHANGE TO OVERRIDE! */
            if (! fromsongstartup || resetthreads)
            {
                /* ANOTHER THREAD HAS BEEN STARTED BY TUPLE-CHANGE, WHILE WE SLEPT, SO ABORT THIS
                   THREAD AND LET THE LATTER (TUPLE-CHANGE) THREAD UPDATE THE LYRICS!
                */
                filename = String ();
                pthread_exit (nullptr);
                return nullptr;
            }
        }
    }

    pthread_mutex_lock (& mutex);

    String cover_helper = aud_get_str ("audacious", "cover_helper");

    if (! resetthreads && cover_helper && cover_helper[0]) //JWT:WE HAVE A PERL HELPER TO LOOK UP COVER ART.
    {
        Tuple tuple = aud_drct_get_tuple ();
        String Title = tuple.get_str (Tuple::Title);
        String Artist = tuple.get_str (Tuple::Artist);
        String Album = tuple.get_str (Tuple::Album);
        String audio_fn = tuple.get_str (Tuple::AudioFile);
        if (! audio_fn || ! audio_fn[0])
            audio_fn = aud_drct_get_filename ();

        const char * album = (const char *) Album;
        if (Title && Title[0])
        {
            if (album && album[0])  // ALBUM FIELD NOT BLANK AND NOT A FILE/URL:
            {
                const char * album_uri = strstr (album, "://");  // FOR URI, WE'LL ASSUME LONGEST IS "stdin" (5 chars)
                if (album_uri && (album_uri-album) < 6)  // ALBUM FIELD IS A URI (PBLY A PODCAST/VIDEO FROM STREAMFINDER!):
                    Album = String ("_");
                else if (aud_get_bool (nullptr, "split_titles"))
                {
                    /* ALBUM MAY ALSO CONTAIN THE STREAM NAME (IE. "<ALBUM> - <STREAM NAME>"): STRIP THAT OFF: */
                    const char * throwaway = strstr (album, " - ");
                    int albumlen = throwaway ? throwaway - album : -1;
                    Album = String (str_copy (album, albumlen));
                }
            }
            else
                Album = String ("_");

            const char * webfetch = (! strncmp (audio_fn, "file://", 7)
                            && aud_get_bool ("albumart", "save_by_songfile"))
                            ? audio_fn : aud_get_str (nullptr, "_cover_art_link");

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
                    (const char *) artist_buf, "' '", (const char *) title_buf, "' ",
                    webfetch}), SW_HIDE);
#else
            system ((const char *) str_concat ({cover_helper, " ALBUM '",
                    (const char *) album_buf, "' ", aud_get_path (AudPath::UserDir), " '",
                    (const char *) artist_buf, "' '", (const char *) title_buf, "' ",
                    webfetch}));
#endif
        }
    }
    else
        abortthisthread = true;

    cover_helper = String ();

    if (! abortthisthread && ! resetthreads)
    {
        skipArtReInit = false;
        g_idle_add (albumart_ready, data);
    }

    pthread_mutex_unlock (& mutex);

    pthread_exit (nullptr);

    return nullptr;
}

/* JWT:UPDATE THE ALBUM-COVER IMAGE (CALL THREAD IF DYNAMIC ALBUM-ART OPTION IN EFFECT): */
static void album_update (void *, GtkWidget * widget)
{
    bool haveartalready = false;
    bool hookalreadycalled = false;  // ONLY CALL HOOK ONCE BEFORE SEARCHING CACHE/WEB FOR ALBUM-COVER.
    int hide_channel_art = 0;
    bool skip_web_art_search = aud_get_bool (nullptr, "_skip_web_art_search");
    String filename = aud_drct_get_filename ();

    if (skip_web_art_search)
        aud_set_bool (nullptr, "_skip_web_art_search", false);

    if (skipArtReInit)
        skipArtReInit = false;
    else
    {
        bool have_dir_icon_art = false;  // TRUE IF/WHEN WE FIND A DIRECTORY "CHANNEL-ICON".

        AudguiPixbuf pixbuf = audgui_pixbuf_request_current ();
        if (pixbuf)
        {
            haveartalready = true;  // WE DO HAVE AN ART IMAGE!
            hide_channel_art = 1;   // TELL infoarea TO HIDE DUPS (UNLESS WE LATER FIND ONE ON THE WEB).
        }
        if (! strncmp (filename, "file://", 7)
                && aud_get_bool ("albumart", "seek_directory_channel_art"))
        {
            /* FOR LOCAL FILES W/O CHANNEL ART, LOOK FOR A DIRECTORY CHANNEL ICON FILE: */
            String dir_channel_icon = aud_get_str ("albumart", "directory_channel_art");
            if (dir_channel_icon && dir_channel_icon[0])
            {
                struct stat statbuf;
                StringBuf icon_path = str_concat ({filename_get_parent (uri_to_filename (filename)), "/"});
                StringBuf icon_fid = str_concat ({icon_path, dir_channel_icon});
                const char * filename;
                const char * ext;
                int isub_p;
                uri_parse (icon_fid, & filename, & ext, nullptr, & isub_p);
                if (! ext || ! ext[0])
                {
                    Index<String> extlist = str_list_to_index ("jpg,png,jpeg", ",");
                    for (auto & ext : extlist)
                    {
                        dir_channel_icon = String (str_concat ({icon_fid, ".", (const char *) ext}));
                        struct stat statbuf;
                        if (stat ((const char *) dir_channel_icon, &statbuf) < 0)  // ART IMAGE FILE DOESN'T EXIST:
                            dir_channel_icon = String ("");
                        else
                            break;
                    }
                }
                else
                    dir_channel_icon = String (icon_fid);

                if (! pixbuf && dir_channel_icon && dir_channel_icon[0]
                        && stat ((const char *) dir_channel_icon, & statbuf) == 0)
                {
                    pixbuf = audgui_pixbuf_request ((const char *) dir_channel_icon);
                    if (pixbuf)
                    {
                        /* JWT:WE HAVE A DIRECTORY ICON, BUT TREAT DIRECTORY ICON AS PRIMARY ART SINCE
                           IT'S THE *ONLY* IMAGE (THUS A DUP.), BUT WE MAY FIND ONE LATER ON THE WEB,
                           WHICH WILL CAUSE IT TO BE SHOWN AS A "CHANNEL-ART" IMAGE IN THAT CASE):
                        */
                        have_dir_icon_art = true;
                        hide_channel_art = 2;  // FORCE HIDE, SINCE CAN'T HAVE CHANNEL ART IF DIR ART'S THE MAIN IMAGE!
                    }
                }
            }
        }
        if (! pixbuf && ! have_dir_icon_art)
            pixbuf = audgui_pixbuf_fallback ();

        if (pixbuf)
            audgui_scaled_image_set (widget, pixbuf.get ());
    }

    if (aud_get_bool ("albumart", "hide_dup_art_icon") && (! strncmp (filename, "http://", 7)
            || ! strncmp (filename, "https://", 8)))  // WE'RE A STREAM (THAT CAN CHANGE TITLES/ART LATER)!:
    {
        /* HOOK TO HIDE (ASSUME WE HAVE NO ART - YET - ANY EXISTING ART IS PBLY FROM PREV. TITLE):
           WE WILL SEARCH CACHE, AND WEB. LATER IF NEEDED, AHD HOOK AGAIN TO SHOW IF ART FOUND:
        */
        aud_set_int ("albumart", "_infoarea_hide_art_gtk", 1);
        hook_call ("gtkui toggle infoarea_art", nullptr);
        hookalreadycalled = true;
    }

    last_image_from_web = false;
    if (haveartalready)  /* JWT:IF SONG IS A FILE & ALREADY HAVE ART IMAGE, SKIP FURTHER ART SEARCH! */
    {
        if (! strncmp (filename, "file://", 7)
                || (! strncmp (filename, "cdda://", 7) && ! aud_get_bool ("CDDA", "seek_albumart_for_cds"))
                || (! strncmp (filename, "dvd://", 6) && aud_get_bool ("dvd", "skip_coverartlookup")))
        {
            if (aud_get_bool ("albumart", "hide_dup_art_icon"))
            {
                aud_set_int ("albumart", "_infoarea_hide_art_gtk", hide_channel_art);
                hook_call ("gtkui toggle infoarea_art", nullptr);
            }
            return;
        }
    }

    /* JWT:NOW CHECK THE ALBUM-ART CACHE
       (ANY ART FOUND BELOW HERE IS UNIQUE TO TITLE AND THUS NOT A DUP.)
       NOTE:  IF HERE, WE'RE A STREAM (OR A LOCAL FILE WITH NO ART, B/C THE CACHE HAS ALREADY BEEN
       CHECKED FOR LOCAL FILES):
    */
    Tuple tuple = aud_drct_get_tuple ();
    String Title = tuple.get_str (Tuple::Title);
    String Artist = tuple.get_str (Tuple::Artist);
    String Album = tuple.get_str (Tuple::Album);
    String audio_fn = tuple.get_str (Tuple::AudioFile);
    if (! audio_fn || ! audio_fn[0])
        audio_fn = aud_drct_get_filename ();

    const char * album = (const char *) Album;

    if (Title && Title[0])
    {
        bool split_titles = aud_get_bool (nullptr, "split_titles");
        bool skipweb = false;
        if (album && album[0])  // ALBUM FIELD NOT BLANK AND NOT A FILE/URL:
        {
            const char * album_uri = strstr (album, "://");  // FOR URI, WE'LL ASSUME LONGEST IS "stdin" (5 chars)
            if (album_uri && (album_uri-album) < 6)  // ALBUM FIELD IS A URI (PBLY A PODCAST/VIDEO FROM STREAMFINDER!):
            {
                Album = String ("_");
                String s = aud_get_str (nullptr, "_cover_art_link");
                if (! s || ! s[0])
                    skipweb = true;
            }
            else if (split_titles)
            {
                /* ALBUM MAY ALSO CONTAIN THE STREAM NAME (IE. "<ALBUM> - <STREAM NAME>"): STRIP THAT OFF: */
                const char * throwaway = strstr (album, " - ");
                int albumlen = throwaway ? throwaway - album : -1;
                Album = String (str_copy (album, albumlen));
            }
        }
        else
            Album = String ("_");

        if (! split_titles)
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

        StringBuf albart_FN;
        StringBuf album_buf = str_copy (Album);
        str_replace_char (album_buf, ' ', '~');  // JWT:PROTECT SPACES, WHICH I STUPIDLY DIDN'T ENCODE IN ALBUMART!
        if (Artist && Artist[0])
        {
            StringBuf artist_buf = str_copy (Artist);
            str_replace_char (artist_buf, ' ', '~');
            albart_FN = str_concat ({(const char *) str_encode_percent (album_buf), "__",
                    (const char *) str_encode_percent (artist_buf)});
        }
        else
        {
            if (Album == String ("_"))
            {
                if (! hookalreadycalled)
                {
                    /* IF HERE, WE'RE A FILE W/NO ART, SO HOOK TO HIDE REGARDLESS OF DIRECTORY-ICON OR NOT: */
                    aud_set_int ("albumart", "_infoarea_hide_art_gtk", 3);
                    hook_call ("gtkui toggle infoarea_art", nullptr);
                }
                return;   /* JWT:NO ALBUM OR ARTIST, PUNT (MAY BE STREAM OR FILE)! */
            }
            else if (Title && Title[0])
            {
                StringBuf title_buf = str_copy (Title);
                str_replace_char (title_buf, ' ', '~');
                albart_FN = str_concat ({(const char *) str_encode_percent (album_buf), "__",
                        (const char *) str_encode_percent (title_buf)});
            }
            else
                albart_FN = str_encode_percent (album_buf);
        }
        str_replace_char (albart_FN, '~', ' ');  // JWT:UNPROTECT SPACES!

        String coverart_file;
        Index<String> extlist = str_list_to_index ("jpg,png,gif,jpeg", ",");
        AudguiPixbuf pixbuf;
        for (auto & ext : extlist)
        {
            coverart_file = String (str_concat ({aud_get_path (AudPath::UserDir),
                    "/albumart/", (const char *) albart_FN, ".", (const char *) ext}));
            const char * filenamechar = coverart_file;
            struct stat statbuf;
            if (stat (filenamechar, &statbuf) >= 0)  // ART IMAGE FILE EXISTS:
            {
                String coverart_uri = String (filename_to_uri (filenamechar));
                pixbuf = audgui_pixbuf_request ((const char *) coverart_uri);
                if (pixbuf)  /* FOUND ART IN CACHE (THUS NOT A DUP.), RETURN: */
                {
                    audgui_scaled_image_set ((GtkWidget *) widget, pixbuf.get ());
                    /* MAKE FILE NEWEST FOR EASIER USER-LOOKUP IN SONG-EDIT!: */
                    if (utime (filenamechar, nullptr) < 0)
                        AUDWARN ("i:Failed to update art-file time (for easier user-lookup)!\n");

                    /* INFOBAR ICON POSSIBLY HIDDEN BY HIDE DUP. OPTION, SO FORCE "SHOW" IN INFOBAR: */
                    aud_set_int ("albumart", "_infoarea_hide_art_gtk", 0);
                    hook_call ("gtkui toggle infoarea_art", nullptr);
                    last_image_from_web = true;

                    return;
                }
                else
                    break;
            }
        }

        /* JWT:NO CACHED ART, CALL HELPER: */
        if (! skip_web_art_search && ! skipweb && aud_get_bool ("albumart", "internet_coverartlookup")
                && aud_get_str ("audacious", "cover_helper"))
        {
            pthread_attr_t thread_attrs;
            if (! pthread_attr_init (& thread_attrs))
            {
                if (! pthread_attr_setdetachstate (& thread_attrs, PTHREAD_CREATE_DETACHED)
                        || ! pthread_attr_setscope (& thread_attrs, PTHREAD_SCOPE_PROCESS))
                {
                    pthread_t album_helper_thread;

                    resetthreads = false;
                    if (pthread_create (&album_helper_thread, nullptr, album_helper_thread_fn, widget))
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
}

/* JWT:CALLED WHEN SONG ENTRY CHANGES: */
static void album_init (void *, GtkWidget * widget)
{
    aud_set_str (nullptr, "_cover_art_link", "");  // JWT:MAKE SURE THIS IS CLEARED, AS THREAD DOESN'T ALWAYS SEEM TO DO SO?!:
    resetthreads = true;
    fromsongstartup = true;
    album_update (nullptr, widget);  // JWT:CHECK FILES & DISKS (TUPLE DOESN'T CHANGE IN THESE) ONCE NOW ON PLAY START!
}

/* JWT:CALLED WHEN TITLE CHANGES WITHIN THE SAME SONG/STREAM ENTRY: */
static void album_tuplechg (void *, GtkWidget * widget)
{
    fromsongstartup = false;
    album_update (nullptr, widget);
}

/* JWT:CALLED WHEN PLAY IS STOPPED (BUT NOT WHEN JUMPING BETWEEN ENTRIES: */
static void album_clear (void *, GtkWidget * widget)
{
    resetthreads = true;
    audgui_scaled_image_set (widget, nullptr);
}

/* JWT:CALLED WHEN USER TOOGLES THE hide_dup_art_icon CHECKBOX: */
/* IF ON, WE HIDE THE "DUPLICATE" IMG. IN INFOBAR, UNLESS WE FETCHED AN IMG. FROM THE WEB! */
/* (THIS OPTION HAS NO EFFECT UNLESS BOTH THE "VIEW - SHOW INFOBAR ALBUM ART" -AND- */
/* THE PLUGIN'S "LOOK FOR ALBUM ART ON THE WEB" OPTIONS ARE BOTH ON)! */
static void hide_dup_art_icon_toggle_fn ()
{
    aud_set_bool ("albumart", "hide_dup_art_icon", hide_dup_art_icon);
    if (hide_dup_art_icon)
    {
        int infoarea_hide_art_gtk_prev = aud_get_int ("albumart", "_infoarea_hide_art_gtk_prev");
        if (infoarea_hide_art_gtk_prev >= 0)
            aud_set_int ("albumart", "_infoarea_hide_art_gtk", infoarea_hide_art_gtk_prev);
    }
    hook_call ("gtkui toggle infoarea_art", nullptr);
}

static void album_cleanup (GtkWidget * widget)
{
    resetthreads = true;
    aud_set_bool ("albumart", "_isactive", false);
    hook_call ("gtkui toggle infoarea_art", nullptr);

    hook_dissociate ("playback stop", (HookFunction) album_clear, widget);
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
    hook_associate ("playback stop", (HookFunction) album_clear, widget);

    aud_set_bool ("albumart", "_isactive", true);

    if (aud_drct_get_ready ())
        album_init (nullptr, widget);

    return widget;
}

/*  DEPRECIATED: JWT:FIXME: THIS IS MARKED "EXPERIMENTAL" IN WINDOWS SINCE GUI-INTERACTION CAN
    BECOME INVISIBLE AFTER A TIME UNTIL PLAY STOPPED & RESTARTED LEADING TO A BAD
    USER-EXPERIENCE, AND I HAVEN'T BEEN ABLE TO FIGURE OUT WHY?!?!?!
    UPDATE:  AS OF v4.1.2-final, THIS ISSUE SEEMS TO BE RESOLVED!
*/

const PreferencesWidget AlbumArtPlugin::widgets[] = {
    WidgetLabel(N_("<b>Albumart Configuration</b>")),
    WidgetCheck (N_("Look for album art on the web."),
        WidgetBool ("albumart", "internet_coverartlookup")),
    WidgetCheck (N_("Hide info bar art icon unless separate album cover fetched."),
        WidgetBool (hide_dup_art_icon, hide_dup_art_icon_toggle_fn)),
    WidgetCheck (N_("Scale small images to fill."),
        WidgetBool ("albumart", "scale_to_fill")),
    WidgetCheck (N_("Try to save by song file-name first?"),
        WidgetBool ("albumart", "save_by_songfile")),
};

const PluginPreferences AlbumArtPlugin::prefs = {{widgets}};
