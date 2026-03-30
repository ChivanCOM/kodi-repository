"""
iBroadcast Music - Kodi Plugin
Entry point and URL router.
"""

import sys
import os
import threading
import urllib.parse

import xbmc
import xbmcgui
import xbmcplugin
import xbmcaddon

try:
    import xbmcvfs
    _translate = xbmcvfs.translatePath
except (ImportError, AttributeError):
    _translate = xbmc.translatePath

ADDON = xbmcaddon.Addon()
HANDLE = int(sys.argv[1])
BASE_URL = sys.argv[0]
PROFILE_PATH = _translate(ADDON.getAddonInfo("profile"))

def get_bitrate():
    """Return the selected bitrate setting (e.g. '128' or 'orig')."""
    return ADDON.getSetting("bitrate") or "128"

sys.path.insert(0, os.path.join(ADDON.getAddonInfo("path"), "lib"))
from ibroadcast import IBroadcastAPI, IBroadcastError
from metadata import MetadataClient


def _get_meta():
    return MetadataClient(
        PROFILE_PATH,
        fanart_api_key=ADDON.getSetting("fanart_tv_api_key") or "",
    )


def _run_prefetch_bg(api, force, profile_path, ftv_key, icon):
    """Background worker: fetch metadata for all artists and albums without blocking the UI.

    All Kodi addon-API values (profile_path, ftv_key, icon) are pre-resolved
    in the main thread before this thread starts, so no ADDON.* calls are made
    here — those C-extension calls can crash if the plugin context is collected
    while a long rebuild is still running.
    """
    try:
        meta    = MetadataClient(profile_path, fanart_api_key=ftv_key)
        monitor = xbmc.Monitor()

        artists = [(a["id"], a["name"]) for a in api.get_artists()]
        albums  = [
            (alb["id"], api.get_artist_name(alb["artist_id"]), alb["name"])
            for alb in api.get_albums()
            if api.get_artist_name(alb["artist_id"])
        ]

        def cancelled():
            return monitor.abortRequested()

        if force:
            meta.clear_cache()

        fa,  sa  = meta.prefetch_artists(artists, is_cancelled=cancelled, force=force)
        fal, sal = meta.prefetch_albums(albums,   is_cancelled=cancelled, force=force)

        if not monitor.abortRequested():
            xbmc.log(
                f"[iBroadcast/meta] prefetch done: {fa} artists, {fal} albums fetched; "
                f"{sa + sal} skipped",
                xbmc.LOGINFO,
            )
            # xbmc.executebuiltin routes through Kodi's main event loop and is more
            # reliable from background threads than xbmcgui.Dialog().notification().
            # Commas delimit Notification() args so use '/' as separator.
            xbmc.executebuiltin(
                f"Notification(iBroadcast,"
                f"Metadata complete: {fa} artists / {fal} albums,"
                f"8000,"
                f"{icon})"
            )
    except Exception as e:
        try:
            xbmc.log(f"[iBroadcast/meta] prefetch error: {e}", xbmc.LOGERROR)
        except Exception:
            pass


def _prefetch_metadata(api, force=False):
    """Start metadata prefetch in a background thread and return immediately."""
    # Resolve all Kodi addon-API values here, in the main thread, before the
    # thread starts.  The background thread must not call ADDON.* methods
    # because Kodi may clean up the plugin's addon context while the thread
    # is still running (full rebuilds can take 30+ minutes).
    icon         = ADDON.getAddonInfo("path") + "/icon.png"
    profile_path = PROFILE_PATH
    ftv_key      = ADDON.getSetting("fanart_tv_api_key") or ""
    t = threading.Thread(
        target=_run_prefetch_bg,
        args=(api, force, profile_path, ftv_key, icon),
        daemon=False,
    )
    t.start()
    label = "Rebuilding metadata in background…" if force else "Updating metadata in background…"
    xbmcgui.Dialog().notification("iBroadcast", label, xbmcgui.NOTIFICATION_INFO, 3000)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def build_url(mode, **kwargs):
    params = {"mode": mode}
    params.update(kwargs)
    return BASE_URL + "?" + urllib.parse.urlencode(params)


def _get_saved_api():
    """Return an API instance loaded with credentials from Kodi settings, or None."""
    token   = ADDON.getSetting("token")
    user_id = ADDON.getSetting("user_id")
    api = IBroadcastAPI(PROFILE_PATH, token=token, user_id=user_id)
    return api if api.is_authenticated() else None


def _save_credentials(api):
    ADDON.setSetting("token",   api.token)
    ADDON.setSetting("user_id", str(api.user_id))


def _clear_credentials():
    ADDON.setSetting("token",   "")
    ADDON.setSetting("user_id", "")


def _keyboard_login():
    """Prompt for email and password via keyboard and attempt login. Returns the API on success."""
    kb = xbmc.Keyboard("", "iBroadcast — Email Address")
    kb.doModal()
    if not kb.isConfirmed():
        return None
    email = kb.getText().strip()
    if not email:
        return None

    kb = xbmc.Keyboard("", "iBroadcast — Password")
    kb.setHiddenInput(True)
    kb.doModal()
    if not kb.isConfirmed():
        return None
    password = kb.getText()
    if not password:
        return None

    api = IBroadcastAPI(PROFILE_PATH)
    ok, msg = api.login(email, password)
    if ok:
        _save_credentials(api)
        return api
    xbmcgui.Dialog().ok("iBroadcast", f"Login failed:\n{msg}")
    return None


def get_api(require_library=False):
    """Return an authenticated API instance, prompting for login if needed."""
    api = _get_saved_api()

    if not api:
        api = _keyboard_login()
        if not api:
            return None

    if require_library:
        if not api.load_library():
            xbmcgui.Dialog().ok("iBroadcast", "Failed to load library. Check your connection.")
            return None

    return api


def end_directory(content_type=None, succeeded=True):
    if content_type:
        xbmcplugin.setContent(HANDLE, content_type)
    xbmcplugin.endOfDirectory(HANDLE, succeeded=succeeded)


# ---------------------------------------------------------------------------
# Views
# ---------------------------------------------------------------------------

def main_menu():
    items = [
        ("Artists",         build_url("artists"),   True),
        ("Albums",          build_url("albums"),    True),
        ("Playlists",       build_url("playlists"), True),
        ("All Tracks",      build_url("tracks"),    True),
        ("Search",          build_url("search"),    True),
        ("Refresh Library",   build_url("refresh"),  False),
    ]
    for label, url, is_folder in items:
        li = xbmcgui.ListItem(label=label)
        li.setArt({"icon": "DefaultMusicAlbums.png"})
        xbmcplugin.addDirectoryItem(HANDLE, url, li, is_folder)
    end_directory()


def list_artists():
    api = get_api(require_library=True)
    if not api:
        return

    meta = _get_meta()
    xbmcplugin.setContent(HANDLE, "artists")
    for artist in api.get_artists():
        li = xbmcgui.ListItem(label=artist["name"])

        ib_art = api.get_artwork_url(artist.get("artwork_id"))
        art  = {"thumb": ib_art, "icon": ib_art} if ib_art else {"icon": "DefaultArtist.png"}
        info = {"artist": artist["name"], "mediatype": "artist"}

        cached = meta.get_artist_info_cached(artist["id"])
        if cached:
            if cached.get("thumb") and not ib_art:
                art["thumb"] = art["icon"] = cached["thumb"]
            # poster: prefer TADB/FTV portrait photo over iBroadcast thumb
            art["poster"] = cached.get("thumb") or art.get("thumb") or ""
            if not art["poster"]: del art["poster"]
            if cached.get("fanart"):     art["fanart"]    = cached["fanart"]
            # widethumb is a native 16:9 image — better landscape source than fanart
            if cached.get("widethumb"): art["landscape"] = cached["widethumb"]
            elif cached.get("fanart"):  art["landscape"] = cached["fanart"]
            if cached.get("fanart2"):   art["fanart2"]   = cached["fanart2"]
            if cached.get("fanart3"):   art["fanart3"]   = cached["fanart3"]
            if cached.get("fanart4"):   art["fanart4"]   = cached["fanart4"]
            if cached.get("clearlogo"): art["clearlogo"] = cached["clearlogo"]
            if cached.get("clearart"):  art["clearart"]  = cached["clearart"]
            if cached.get("banner"):    art["banner"]    = cached["banner"]
            if cached.get("genre"):     info["genre"]    = cached["genre"]
            if cached.get("mbid"):      info["musicbrainzartistid"] = cached["mbid"]

        li.setInfo("music", info)
        li.setArt(art)
        if cached:
            if cached.get("biography"):  li.setProperty("Artist_Description", cached["biography"])
            if cached.get("style"):      li.setProperty("Artist_Style",        cached["style"])
            if cached.get("mood"):       li.setProperty("Artist_Mood",         cached["mood"])
            if cached.get("born_year"):  li.setProperty("Artist_Born",         str(cached["born_year"]))
            if cached.get("country"):    li.setProperty("Artist_Country",      cached["country"])
        li.addContextMenuItems([
            (ADDON.getLocalizedString(32037),
             f"RunPlugin({build_url('fetch_meta_artist', artist_id=artist['id'])})"),
            (ADDON.getLocalizedString(32038),
             f"RunPlugin({build_url('fetch_meta_artist', artist_id=artist['id'], custom=1)})"),
        ])
        xbmcplugin.addDirectoryItem(HANDLE, build_url("artist_albums", artist_id=artist["id"]), li, True)

    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_LABEL_IGNORE_THE)
    end_directory("artists")


def list_albums(artist_id=None):
    api = get_api(require_library=True)
    if not api:
        return

    meta   = _get_meta()
    albums = api.get_albums(artist_id=artist_id)

    # Pre-load single artist meta when filtering by artist; otherwise look up per album below
    _fixed_artist_meta = meta.get_artist_info_cached(artist_id) if artist_id else {}
    _artist_meta_cache = {}  # dedup reads when browsing all albums

    xbmcplugin.setContent(HANDLE, "albums")
    for album in albums:
        artist_name = api.get_artist_name(album["artist_id"])
        alb_meta    = meta.get_album_info_cached(album["id"])

        # Per-album artist meta (needed in "all albums" view where artist changes per item)
        if artist_id:
            artist_meta = _fixed_artist_meta
        else:
            aid = album["artist_id"]
            if aid not in _artist_meta_cache:
                _artist_meta_cache[aid] = meta.get_artist_info_cached(aid)
            artist_meta = _artist_meta_cache[aid]

        li = xbmcgui.ListItem(label=album["name"])
        info = {"album": album["name"], "artist": artist_name, "mediatype": "album"}
        if album.get("year"):
            info["year"] = int(album["year"])
        if album.get("plays"):
            info["playcount"] = int(album["plays"])
        if album.get("rating"):
            info["rating"] = float(album["rating"])
        if album.get("uploaded_on"):
            # SORT_METHOD_DATE expects "DD.MM.YYYY" in the 'date' info label
            try:
                parts = str(album["uploaded_on"]).split(" ")[0].split("-")
                info["date"] = f"{parts[2]}.{parts[1]}.{parts[0]}"
            except (IndexError, ValueError):
                pass
        genre = alb_meta.get("genre") or artist_meta.get("genre") or ""
        if genre:                   info["genre"]   = genre
        if alb_meta.get("description"):   info["comment"]                  = alb_meta["description"]
        if alb_meta.get("rating"):        info["rating"]                   = float(alb_meta["rating"])
        if alb_meta.get("mbid"):                info["musicbrainzalbumid"]        = alb_meta["mbid"]
        ar_mbid = alb_meta.get("artist_mbid") or artist_meta.get("mbid")
        if ar_mbid:                             info["musicbrainzartistid"]       = ar_mbid
        if ar_mbid:                             info["musicbrainzalbumartistid"]  = ar_mbid

        art_url = api.get_artwork_url(album.get("artwork_id"))
        art = {"thumb": art_url, "icon": art_url} if art_url else {}
        if not art_url and alb_meta.get("thumb"): art["thumb"] = art["icon"] = alb_meta["thumb"]
        if not art: art["icon"] = "DefaultAlbumCover.png"
        # poster: prefer TADB HQ album cover over iBroadcast thumb
        poster = alb_meta.get("thumb") or art.get("thumb")
        if poster: art["poster"] = poster
        if alb_meta.get("discart"):      art["discart"]   = alb_meta["discart"]
        if alb_meta.get("back"):         art["back"]      = alb_meta["back"]
        if artist_meta.get("fanart"):    art["fanart"]    = artist_meta["fanart"]
        if artist_meta.get("fanart"):    art["landscape"] = artist_meta["fanart"]
        if artist_meta.get("fanart2"):   art["fanart2"]   = artist_meta["fanart2"]
        if artist_meta.get("fanart3"):   art["fanart3"]   = artist_meta["fanart3"]
        if artist_meta.get("clearlogo"): art["clearlogo"] = artist_meta["clearlogo"]
        if artist_meta.get("clearart"):  art["clearart"]  = artist_meta["clearart"]
        if artist_meta.get("banner"):    art["banner"]    = artist_meta["banner"]

        li.setInfo("music", info)
        li.setArt(art)
        if alb_meta.get("description"): li.setProperty("Album_Description", alb_meta["description"])
        if alb_meta.get("style"):       li.setProperty("Album_Style",        alb_meta["style"])
        if alb_meta.get("mood"):        li.setProperty("Album_Mood",         alb_meta["mood"])
        if alb_meta.get("theme"):       li.setProperty("Album_Theme",        alb_meta["theme"])
        if alb_meta.get("rating"):      li.setProperty("Album_Rating",       str(alb_meta["rating"]))
        li.addContextMenuItems([
            (ADDON.getLocalizedString(32037),
             f"RunPlugin({build_url('fetch_meta_album', album_id=album['id'])})"),
            (ADDON.getLocalizedString(32038),
             f"RunPlugin({build_url('fetch_meta_album', album_id=album['id'], custom=1)})"),
        ])
        xbmcplugin.addDirectoryItem(
            HANDLE, build_url("album_tracks", album_id=album["id"]), li, True
        )

    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_ALBUM_IGNORE_THE)
    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_LABEL_IGNORE_THE)
    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_DATE)
    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_SONG_RATING)
    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_PLAYCOUNT)
    end_directory("albums")


def list_tracks(album_id=None, artist_id=None, playlist_id=None):
    api = get_api(require_library=True)
    if not api:
        return

    meta   = _get_meta()
    tracks = api.get_tracks(album_id=album_id, artist_id=artist_id, playlist_id=playlist_id)

    # Preload album and artist metadata for every unique id in this tracklist.
    # track["artist_id"] is always the album's primary artist (set in ibroadcast.py),
    # so every track gets the correct fanart/logo regardless of view type.
    _album_meta_cache  = {}
    _artist_meta_cache = {}
    for t in tracks:
        aid = t.get("album_id")
        if aid is not None and aid not in _album_meta_cache:
            _album_meta_cache[aid]  = meta.get_album_info_cached(aid)
        ar_id = t.get("artist_id")
        if ar_id is not None and ar_id not in _artist_meta_cache:
            _artist_meta_cache[ar_id] = meta.get_artist_info_cached(ar_id)

    xbmcplugin.setContent(HANDLE, "songs")
    for track in tracks:
        album_meta  = _album_meta_cache.get(track.get("album_id"), {})
        artist_meta = _artist_meta_cache.get(track.get("artist_id"), {})

        artist_name = api.get_artist_name(track["artist_id"])
        album_name  = api.get_album_name(track["album_id"])
        li = xbmcgui.ListItem(label=track["title"])
        info = {
            "title":       track["title"],
            "artist":      artist_name,
            "album":       album_name,
            "tracknumber": track["track_number"],
            "duration":    track["duration"],
            "genre":       track["genre"] or album_meta.get("genre", ""),
            "mediatype":   "song",
        }
        if track.get("year"):
            info["year"] = int(track["year"])
        if track.get("plays"):
            info["playcount"] = int(track["plays"])
        if track.get("rating"):
            info["rating"] = float(track["rating"])
        if track.get("uploaded_on"):
            try:
                parts = str(track["uploaded_on"]).split(" ")[0].split("-")
                info["date"] = f"{parts[2]}.{parts[1]}.{parts[0]}"
            except (IndexError, ValueError):
                pass
        if album_meta.get("description"):  info["comment"]                 = album_meta["description"]
        if album_meta.get("rating"):       info["rating"]                  = float(album_meta["rating"])
        if album_meta.get("mbid"):            info["musicbrainzalbumid"]        = album_meta["mbid"]
        ar_mbid = album_meta.get("artist_mbid") or artist_meta.get("mbid")
        if ar_mbid:                           info["musicbrainzartistid"]       = ar_mbid
        if ar_mbid:                           info["musicbrainzalbumartistid"]  = ar_mbid
        li.setInfo("music", info)
        if album_meta.get("description"): li.setProperty("Album_Description", album_meta["description"])
        if album_meta.get("style"):       li.setProperty("Album_Style",        album_meta["style"])
        if album_meta.get("mood"):        li.setProperty("Album_Mood",         album_meta["mood"])
        if album_meta.get("theme"):       li.setProperty("Album_Theme",        album_meta["theme"])
        # Track-level artist credit (e.g. "Flying Lotus, George Clinton") when it
        # differs from the album artist — stored as a property for skins that show it
        track_artist_id = track.get("track_artist_id")
        if track_artist_id and track_artist_id != track.get("artist_id"):
            track_artist_name = api.get_artist_name(track_artist_id)
            if track_artist_name:
                li.setProperty("Track_Artist", track_artist_name)

        art_url = api.get_artwork_url(track.get("artwork_id"))
        art = {"thumb": art_url, "icon": art_url} if art_url else {}
        # Fallback thumb: TADB/FTV album cover when iBroadcast has no artwork for this track
        if not art_url and album_meta.get("thumb"):
            art["thumb"] = art["icon"] = album_meta["thumb"]
        if art.get("thumb"):
            art["poster"] = art["thumb"]
        if album_meta.get("discart"):    art["discart"]   = album_meta["discart"]
        if album_meta.get("back"):       art["back"]      = album_meta["back"]
        # Fanart + landscape from artist meta (richer source); album_meta fanart is FTV artist bg too
        fanart = artist_meta.get("fanart") or album_meta.get("fanart")
        if fanart:                       art["fanart"]    = fanart
        landscape = artist_meta.get("widethumb") or fanart
        if landscape:                    art["landscape"] = landscape
        if artist_meta.get("fanart2"):   art["fanart2"]   = artist_meta["fanart2"]
        if artist_meta.get("fanart3"):   art["fanart3"]   = artist_meta["fanart3"]
        if artist_meta.get("fanart4"):   art["fanart4"]   = artist_meta["fanart4"]
        if artist_meta.get("clearlogo"): art["clearlogo"] = artist_meta["clearlogo"]
        if artist_meta.get("clearart"):  art["clearart"]  = artist_meta["clearart"]
        if artist_meta.get("banner"):    art["banner"]    = artist_meta["banner"]
        if art:
            li.setArt(art)
        li.setProperty("IsPlayable", "true")
        xbmcplugin.addDirectoryItem(
            HANDLE, build_url("play", track_id=track["id"]), li, False
        )

    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_TRACKNUM)
    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_LABEL_IGNORE_THE)
    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_DATE)
    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_SONG_RATING)
    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_PLAYCOUNT)
    end_directory("songs")


def list_playlists():
    api = get_api(require_library=True)
    if not api:
        return

    for pl in api.get_playlists():
        li = xbmcgui.ListItem(label=pl["name"])
        if pl.get("description"):
            li.setInfo("music", {"comment": pl["description"]})
        li.setArt({"icon": "DefaultMusicPlaylists.png"})
        xbmcplugin.addDirectoryItem(
            HANDLE, build_url("playlist_tracks", playlist_id=pl["id"]), li, True
        )

    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_LABEL_IGNORE_THE)
    end_directory()


def search_tracks():
    api = get_api(require_library=True)
    if not api:
        return

    query = xbmcgui.Dialog().input("Search iBroadcast", type=xbmcgui.INPUT_ALPHANUM)
    if not query:
        end_directory(succeeded=False)
        return

    results = api.search(query)
    if not results:
        xbmcgui.Dialog().notification(
            "iBroadcast", f'No results for "{query}"', xbmcgui.NOTIFICATION_INFO
        )
        end_directory(succeeded=False)
        return

    xbmcplugin.setContent(HANDLE, "songs")
    for track in results:
        artist_name = api.get_artist_name(track["artist_id"])
        album_name = api.get_album_name(track["album_id"])
        label = f"{track['title']} — {artist_name}" if artist_name else track["title"]
        li = xbmcgui.ListItem(label=label)
        info = {
            "title": track["title"],
            "artist": artist_name,
            "album": album_name,
            "tracknumber": track["track_number"],
            "duration": track["duration"],
            "genre": track["genre"],
        }
        if track.get("year"):
            info["year"] = int(track["year"])
        li.setInfo("music", info)
        art_url = api.get_artwork_url(track.get("artwork_id"))
        if art_url:
            li.setArt({"thumb": art_url, "icon": art_url})
        li.setProperty("IsPlayable", "true")
        xbmcplugin.addDirectoryItem(
            HANDLE, build_url("play", track_id=track["id"]), li, False
        )

    end_directory("songs")


def play_track(track_id):
    api = get_api(require_library=True)
    if not api:
        xbmcplugin.setResolvedUrl(HANDLE, False, xbmcgui.ListItem())
        return

    bitrate = get_bitrate()
    stream_url = api.get_stream_url(track_id, bitrate=bitrate)
    if not stream_url:
        xbmcgui.Dialog().notification(
            "iBroadcast", "Could not get stream URL", xbmcgui.NOTIFICATION_ERROR
        )
        xbmcplugin.setResolvedUrl(HANDLE, False, xbmcgui.ListItem())
        return

    li = xbmcgui.ListItem(path=stream_url)

    # Populate now-playing metadata
    tracks = api.get_tracks()
    track = next((t for t in tracks if str(t["id"]) == str(track_id)), None)
    if track:
        artist_name = api.get_artist_name(track["artist_id"])
        album_name = api.get_album_name(track["album_id"])
        info = {
            "title": track["title"],
            "artist": artist_name,
            "album": album_name,
            "tracknumber": track["track_number"],
            "duration": track["duration"],
            "genre": track["genre"],
        }
        if track.get("year"):
            info["year"] = int(track["year"])
        li.setInfo("music", info)
        art_url = api.get_artwork_url(track.get("artwork_id"))
        if art_url:
            li.setArt({"thumb": art_url})

    xbmcplugin.setResolvedUrl(HANDLE, True, li)


def account_action():
    """Single Account button: login if logged out, offer logout if logged in."""
    if ADDON.getSetting("token") and ADDON.getSetting("user_id"):
        if xbmcgui.Dialog().yesno("iBroadcast", "You are logged in. Log out?"):
            _clear_credentials()
            xbmcgui.Dialog().notification(
                "iBroadcast", "Logged out", xbmcgui.NOTIFICATION_INFO
            )
    else:
        api = _keyboard_login()
        if api:
            xbmcgui.Dialog().ok("iBroadcast", "Login successful!")


def refresh_library():
    api = get_api()
    if not api:
        return
    ok = api.load_library(force_refresh=True)
    if not ok:
        xbmcgui.Dialog().ok("iBroadcast", "Failed to refresh library.")
        return
    xbmcgui.Dialog().notification("iBroadcast", "Library refreshed", xbmcgui.NOTIFICATION_INFO)
    _prefetch_metadata(api, force=False)


def rebuild_metadata():
    api = get_api(require_library=True)
    if not api:
        return
    if not xbmcgui.Dialog().yesno(
        "iBroadcast",
        "Re-scrape metadata for all artists and albums?\n"
        "This replaces all cached metadata and may take a while."
    ):
        return
    _prefetch_metadata(api, force=True)


def _kb(default, heading):
    """Show Kodi keyboard and return stripped text, or None if cancelled."""
    kb = xbmc.Keyboard(default, heading)
    kb.doModal()
    return kb.getText().strip() if kb.isConfirmed() else None


def _meta_result_dialog(title, d, artist_fields, album_fields=None):
    """Show a Dialog.ok() summarising which art/info fields were found vs missing."""
    fields = artist_fields if album_fields is None else album_fields
    found   = [label for key, label in fields if d.get(key)]
    missing = [label for key, label in fields if not d.get(key)]
    lines = []
    if found:
        lines.append(f"Found:   {' · '.join(found)}")
    if missing:
        lines.append(f"Missing: {' · '.join(missing)}")
    if not lines:
        lines = ["No metadata found on any provider."]
    xbmcgui.Dialog().ok(f"iBroadcast — {title}", "\n".join(lines))


_ARTIST_FIELDS = [
    ("thumb",      "Thumb"),
    ("fanart",     "Fanart"),
    ("widethumb",  "Landscape"),
    ("clearlogo",  "Logo"),
    ("clearart",   "Clearart"),
    ("banner",     "Banner"),
    ("biography",  "Bio"),
    ("genre",      "Genre"),
]
_ALBUM_FIELDS = [
    ("thumb",       "Cover"),
    ("discart",     "Discart"),
    ("back",        "Back"),
    ("fanart",      "Fanart"),
    ("description", "Description"),
    ("genre",       "Genre"),
]


def fetch_meta_artist():
    """Force-refresh metadata for a single artist.
    custom=1 → show keyboard pre-filled with stored name so user can correct it.
    """
    params    = dict(urllib.parse.parse_qsl(sys.argv[2].lstrip("?")))
    artist_id = params.get("artist_id")
    if not artist_id:
        return
    api = get_api(require_library=True)
    if not api:
        return
    stored_name = api.get_artist_name(artist_id)

    if params.get("custom") == "1":
        name = _kb(stored_name, "Search artist as…")
        if not name:
            return
    else:
        name = stored_name

    meta = _get_meta()
    d = meta.get_artist_info(int(artist_id), name, force=True)
    _meta_result_dialog(name or str(artist_id), d, _ARTIST_FIELDS)


def fetch_meta_album():
    """Force-refresh metadata for a single album.
    custom=1 → show keyboards pre-filled with stored names so user can correct them.
    """
    params   = dict(urllib.parse.parse_qsl(sys.argv[2].lstrip("?")))
    album_id = params.get("album_id")
    if not album_id:
        return
    api = get_api(require_library=True)
    if not api:
        return
    albums = api.get_albums()
    alb = next((a for a in albums if str(a["id"]) == str(album_id)), None)
    if not alb:
        return
    stored_artist = api.get_artist_name(alb["artist_id"])
    stored_album  = alb["name"]

    if params.get("custom") == "1":
        artist_name = _kb(stored_artist, "Search artist as…")
        if not artist_name:
            return
        album_name = _kb(stored_album, "Search album as…")
        if not album_name:
            return
    else:
        artist_name = stored_artist
        album_name  = stored_album

    meta = _get_meta()
    d = meta.get_album_info(int(album_id), artist_name, album_name, force=True)
    _meta_result_dialog(album_name or str(album_id), d, None, _ALBUM_FIELDS)


def metadata_stats():
    """Show an artwork coverage table built from local cache files — no network calls."""
    api = get_api(require_library=True)
    if not api:
        return

    meta     = _get_meta()
    artists  = api.get_artists()   # album artists only
    albums   = api.get_albums()

    # ── artist counters ───────────────────────────────────────────────────
    ar_total = len(artists)
    ar_cached = ar_thumb = ar_landscape = ar_fanart = ar_logo = 0
    ar_clearart = ar_banner = ar_bio = 0

    for a in artists:
        d = meta.get_artist_info_cached(a["id"])
        if not d:
            continue
        ar_cached   += 1
        if d.get("thumb"):                          ar_thumb     += 1
        if d.get("widethumb") or d.get("fanart"):   ar_landscape += 1
        if d.get("fanart"):                         ar_fanart    += 1
        if d.get("clearlogo"):                      ar_logo      += 1
        if d.get("clearart"):                       ar_clearart  += 1
        if d.get("banner"):                         ar_banner    += 1
        if d.get("biography"):                      ar_bio       += 1

    # ── album counters ────────────────────────────────────────────────────
    al_total = len(albums)
    al_cached = al_thumb = al_discart = al_back = al_fanart = al_desc = 0

    for a in albums:
        d = meta.get_album_info_cached(a["id"])
        if not d:
            continue
        al_cached   += 1
        if d.get("thumb"):        al_thumb   += 1
        if d.get("discart"):      al_discart += 1
        if d.get("back"):         al_back    += 1
        if d.get("fanart"):       al_fanart  += 1
        if d.get("description"):  al_desc    += 1

    # ── format ───────────────────────────────────────────────────────────
    def pct(n, total):
        return f"{round(n * 100 / total)}%" if total else "—"

    def row(label, n, total):
        return f"  {label:<13} {n:>4} / {total:<4}  ({pct(n, total):>4})"

    lines = [
        f"ARTISTS — {ar_cached} cached of {ar_total} total",
        row("Thumb",      ar_thumb,     ar_total),
        row("Landscape",  ar_landscape, ar_total),
        row("Fanart",     ar_fanart,    ar_total),
        row("Clearlogo",  ar_logo,      ar_total),
        row("Clearart",   ar_clearart,  ar_total),
        row("Banner",     ar_banner,    ar_total),
        row("Biography",  ar_bio,       ar_total),
        "",
        f"ALBUMS — {al_cached} cached of {al_total} total",
        row("Cover",        al_thumb,   al_total),
        row("Discart",      al_discart, al_total),
        row("Back cover",   al_back,    al_total),
        row("Fanart",       al_fanart,  al_total),
        row("Description",  al_desc,    al_total),
    ]

    xbmcgui.Dialog().textviewer(
        "iBroadcast — Metadata Statistics",
        "\n".join(lines),
    )


# ---------------------------------------------------------------------------
# Router
# ---------------------------------------------------------------------------

def router():
    params = dict(urllib.parse.parse_qsl(sys.argv[2].lstrip("?")))
    mode = params.get("mode")

    if not mode:
        main_menu()
    elif mode == "artists":
        list_artists()
    elif mode == "artist_albums":
        list_albums(artist_id=params.get("artist_id"))
    elif mode == "albums":
        list_albums()
    elif mode == "album_tracks":
        list_tracks(album_id=params.get("album_id"))
    elif mode == "tracks":
        list_tracks()
    elif mode == "playlists":
        list_playlists()
    elif mode == "playlist_tracks":
        list_tracks(playlist_id=params.get("playlist_id"))
    elif mode == "search":
        search_tracks()
    elif mode == "play":
        play_track(params.get("track_id"))
    elif mode == "account":
        account_action()
    elif mode == "refresh":
        refresh_library()
    elif mode == "rebuild_metadata":
        rebuild_metadata()
    elif mode == "metadata_stats":
        metadata_stats()
    elif mode == "fetch_meta_artist":
        fetch_meta_artist()
    elif mode == "fetch_meta_album":
        fetch_meta_album()
    else:
        main_menu()


if __name__ == "__main__":
    router()
