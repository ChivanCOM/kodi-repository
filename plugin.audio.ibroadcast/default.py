"""
iBroadcast Music - Kodi Plugin
Entry point and URL router.
"""

import sys
import os
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
    """Return the selected bitrate as an integer (kbps)."""
    try:
        return int(ADDON.getSetting("bitrate") or 128)
    except ValueError:
        return 128

sys.path.insert(0, os.path.join(ADDON.getAddonInfo("path"), "lib"))
from ibroadcast import IBroadcastAPI, IBroadcastError


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
        ("Refresh Library", build_url("refresh"),   False),
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

    xbmcplugin.setContent(HANDLE, "artists")
    for aid, name in api.get_artists():
        li = xbmcgui.ListItem(label=name)
        li.setInfo("music", {"artist": name})
        li.setArt({"icon": "DefaultArtist.png"})
        xbmcplugin.addDirectoryItem(HANDLE, build_url("artist_albums", artist_id=aid), li, True)

    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_LABEL_IGNORE_THE)
    end_directory("artists")


def list_albums(artist_id=None):
    api = get_api(require_library=True)
    if not api:
        return

    albums = api.get_albums(artist_id=artist_id)
    xbmcplugin.setContent(HANDLE, "albums")
    for album in albums:
        artist_name = api.get_artist_name(album["artist_id"])
        label = album["name"]
        li = xbmcgui.ListItem(label=label)
        info = {"album": album["name"], "artist": artist_name}
        if album.get("year"):
            info["year"] = int(album["year"])
        li.setInfo("music", info)
        art_url = api.get_artwork_url(album.get("artwork_id"))
        if art_url:
            li.setArt({"thumb": art_url, "icon": art_url, "fanart": art_url})
        else:
            li.setArt({"icon": "DefaultAlbumCover.png"})
        xbmcplugin.addDirectoryItem(
            HANDLE, build_url("album_tracks", album_id=album["id"]), li, True
        )

    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_ALBUM_IGNORE_THE)
    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_LABEL_IGNORE_THE)
    end_directory("albums")


def list_tracks(album_id=None, artist_id=None, playlist_id=None):
    api = get_api(require_library=True)
    if not api:
        return

    tracks = api.get_tracks(album_id=album_id, artist_id=artist_id, playlist_id=playlist_id)
    xbmcplugin.setContent(HANDLE, "songs")
    for track in tracks:
        artist_name = api.get_artist_name(track["artist_id"])
        album_name = api.get_album_name(track["album_id"])
        li = xbmcgui.ListItem(label=track["title"])
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

    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_TRACKNUM)
    xbmcplugin.addSortMethod(HANDLE, xbmcplugin.SORT_METHOD_LABEL_IGNORE_THE)
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
    if ok:
        xbmcgui.Dialog().notification(
            "iBroadcast", "Library refreshed", xbmcgui.NOTIFICATION_INFO
        )
    else:
        xbmcgui.Dialog().ok("iBroadcast", "Failed to refresh library.")


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
    else:
        main_menu()


if __name__ == "__main__":
    router()
