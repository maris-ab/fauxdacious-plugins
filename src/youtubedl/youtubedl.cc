/*
 * YTDL/YTDLH Transport for Audacious
 * Copyright 2016 - by Jim Turner
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
#include <stdio.h>
#include <string.h>

#include <libaudcore/audstrings.h>
#include <libaudcore/runtime.h>
#include <libaudcore/i18n.h>
#include <libaudcore/plugin.h>
#include <libaudcore/preferences.h>
#include <libaudcore/probe.h>

/* youtube-dl --no-continue --no-playlist --no-cache-dir --no-progress --no-call-home 
    --youtube-skip-dash-manifest --prefer-ffmpeg -q -f mp4 --no-part https://youtu.be/Uyw8v_YUhD8?t=6m43s  
    -o - 2>/dev/null | fauxdacious -D stdin://-.mp4
*/

static const char ytdl_about[] =
 N_("Youtube-DL Plugin for Fauxdacious\n"
    "Copyright 2016 by Jim Turner <turnerjw784@yahoo.com\n"
    "\n"
    "Provides live-streaming of Youtube and some other videos that\n"
    "require youtube-dl (or other helper program).  youtube-dl\n"
    "is available separately at: https://rg3.github.io/youtube-dl/,\n"
    "User can configure the helper command and it's arguments.\n"
    "The command (default: youtube-dl) preprocesses the url and\n"
    "pipes the stream to Fauxdacious using popen ().\n"
    "\n"
    "Replace the https: with ytdl: in the url to trigger the plugin.\n"
    "\n"
    "Since streaming is live, no seeking is possible, but a config.\n"
    "option echos the stream to a file that can be replayed later.\n"
    "Currently not compatable with Audacious (mainline).\n"
    "Currently not M$-Windows compatable (no popen() support).\n"
    "\n"
    "[youtubedl].metadata_helper can specify a 2nd helper app. to\n"
    "obtain the Title and User metadata (which youtube-dl doesn't.\n"
    "One is provided (youtubedl_metadatahelper.pl), but perl and\n"
    "the perl module WWW::YouTube::Download is required, along\n"
    "with setting [youtubedl].youtubedl_tag_data to TRUE.\n");

static const char * const ytdl_schemes[] = {"ytdl"};

const char * const ytdl_defaults[] = {
    "save_video", "FALSE",
    "video_qsize", "8",
    nullptr
};

const PreferencesWidget ytdl_widgets[] = {
    WidgetLabel (N_("<b>Advanced</b>")),
    WidgetCheck (N_("Save downloaded video to file ([save_video_file])"),
        WidgetBool ("youtubedl", "save_video")),
    WidgetSpin(N_("Video packet queue size"),
        WidgetInt("youtubedl", "video_qsize"), {0, 56, 1})
};

const PluginPreferences ytdl_prefs = {{ytdl_widgets}};

class YTDLTransport : public TransportPlugin
{
public:
    static constexpr PluginInfo info = {
        N_("Youtube-DL Plugin"), 
        PACKAGE,
        ytdl_about,
        & ytdl_prefs
    };

    constexpr YTDLTransport () : TransportPlugin (info, ytdl_schemes) {}

    VFSImpl * fopen (const char * path, const char * mode, String & error);
};

EXPORT YTDLTransport aud_plugin_instance;

class YTDLFile : public VFSImpl
{
public:
    YTDLFile (const char * path);
    ~YTDLFile ();

    struct OpenError {
        String error;
    };

protected:
    int64_t fread (void * ptr, int64_t size, int64_t nmemb);
    int64_t fwrite (const void * buf, int64_t size, int64_t nitems);

    int fseek (int64_t offset, VFSSeekType whence);
    int64_t ftell ();
    bool feof ();

    int ftruncate (int64_t length);
    int64_t fsize ();

    int fflush ();

private:
    int64_t m_pos = 0;           /* Current position in the stream */
    FILE * m_filehandle = NULL;
    FILE * m_savefile = NULL;    /* File to echo video stream out to (optional). */
    String m_filename;
    bool m_eof = false;
};

VFSImpl * YTDLTransport::fopen (const char * path, const char * mode, String & error)
{
    return new YTDLFile (path);
}

YTDLFile::YTDLFile (const char * filename)
{
    if (m_filehandle) {
        return;
    }
//https://youtu.be/Uyw8v_YUhD8?t=6m43s
    String metadata_helper = aud_get_str("youtubedl", "metadata_helper");
    if (metadata_helper[0])
    {
        Tuple file_tuple;
        if (! aud_read_tag_from_tagfile (filename, "tmp_tag_data", file_tuple))
	       {
	           StringBuf tagdata_filename = filename_build ({aud_get_path (AudPath::UserDir), "tmp_tag_data"});
            AUDDBG ("i:invoking metadata helper=%s=\n", (const char *) str_concat ({metadata_helper, " ", filename, " ", tagdata_filename}));
            system ((const char *) str_concat ({metadata_helper, " ", filename, " ", tagdata_filename}));
        }
    }
    else
        aud_set_bool (nullptr, "youtubedl_tag_data", false);  /* NO YOUTUBE-DL TAG DATA FILE W/O HELPER! */

    String youtubedl_Command = aud_get_str("youtubedl", "command");
    if (! youtubedl_Command[0])
        youtubedl_Command = String ("youtube-dl --output /tmp --socket-timeout 420 --embed-thumbnail --no-playlist --no-cache-dir --no-progress --no-call-home --youtube-skip-dash-manifest --prefer-ffmpeg -4 -q -f mp4 --no-part");

    const char * colon = filename ? strrchr (filename, ':') : filename;
    StringBuf pipein = str_printf ("%s %s%s %s", (const char *)youtubedl_Command, "https", colon, " -o - 2>/dev/null");
    m_filehandle = popen ((const char *)pipein, "r");  //see:http://pubs.opengroup.org/onlinepubs/009696799/functions/popen.html
    if (m_filehandle)
    {
    	   if (aud_get_bool("youtubedl", "save_video"))
    	   {
            String save_video_file = aud_get_str("youtubedl", "save_video_file");
            if (! save_video_file[0])
                save_video_file = String ("/tmp/lastyoutubevideo");
        	   m_savefile = fopen ((const char *)save_video_file, "w");
        	   if (! m_savefile)
        	       AUDERR ("e:Could not create file (%s) to save video, will still play...\n");
        }
        return;
    }
    /* Handle error */;
    AUDERR ("e:Failed to open %s.\n", filename);
    return;
}

YTDLFile::~YTDLFile ()
{
    pclose(m_filehandle);
    m_filehandle = nullptr;
    if (m_savefile)
        ::fclose (m_savefile);
}

int64_t YTDLFile::fread (void * buf, int64_t size, int64_t nitems)
{
    if (! m_filehandle || size < 1 || nitems < 1)
    {
        AUDERR ("Cannot read from %s: not open for reading.\n", (const char *) m_filename);
        return 0;
    }

    int64_t bitesread = ::fread (buf, size, nitems, m_filehandle);
    if (bitesread > 0)
        m_pos += bitesread;
    if (m_savefile)
        ::fwrite (buf, size, nitems, m_savefile);
    return bitesread;
}

int64_t YTDLFile::fwrite (const void * data, int64_t size, int64_t count)
{
    AUDERR ("Writing is not supported.\n");
    return 0;
}

int YTDLFile::fseek (int64_t offset, VFSSeekType whence)
{
    AUDDBG ("Seeking is not supported.\n");
    return -1;
}

int64_t YTDLFile::ftell ()
{
    return m_pos;
}

bool YTDLFile::feof ()
{
    return ::feof (m_filehandle);
}

int YTDLFile::ftruncate (int64_t size)
{
    AUDERR ("Truncating is not supported.\n");
    return -1;
}

int64_t YTDLFile::fsize ()
{
    return -1;
}

int YTDLFile::fflush ()
{
    return ::fflush (m_filehandle);
}
