"""
iBroadcast API client for Kodi.

Based on ibroadcastaio (https://github.com/robsonke/ibroadcastaio):
  Login:   POST https://api.ibroadcast.com/s/JSON/status
  Library: POST https://library.ibroadcast.com
  Streaming server and artwork server URLs come from the library response settings.
"""

import json
import os
import re
import time
import urllib.request
import urllib.parse
import urllib.error

try:
    import xbmc
    def _log(msg):
        xbmc.log(f"[iBroadcast] {msg}", xbmc.LOGINFO)
except ImportError:
    def _log(msg):
        print(f"[iBroadcast] {msg}")


class IBroadcastError(Exception):
    pass


class IBroadcastAPI:
    API_URL     = "https://api.ibroadcast.com/s/JSON/status"
    LIBRARY_URL = "https://library.ibroadcast.com"
    CLIENT      = "kodi-plugin"

    def __init__(self, profile_path, token=None, user_id=None):
        self.profile_path       = profile_path  # used for library cache only
        self.token              = token or None
        self.user_id            = user_id or None
        self._library           = None   # dict: track_id/album_id/... → named dict
        self._settings          = {}
        self._streaming_server  = None
        self._artwork_server    = None

    # ------------------------------------------------------------------
    # HTTP
    # ------------------------------------------------------------------

    def _post(self, url, data):
        payload = json.dumps(data).encode("utf-8")
        req = urllib.request.Request(
            url,
            data=payload,
            headers={
                "Content-Type": "application/json",
                "User-Agent": "Mozilla/5.0 (compatible; Kodi/iBroadcast-Plugin/1.0)",
            },
        )
        try:
            with urllib.request.urlopen(req, timeout=30) as resp:
                body = resp.read().decode("utf-8")
                _log(f"POST {url} → {body[:500]}")
                return json.loads(body)
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8")
            _log(f"POST {url} HTTP {e.code} → {body[:500]}")
            try:
                return json.loads(body)
            except Exception:
                raise IBroadcastError(f"HTTP {e.code}: {body}") from e
        except urllib.error.URLError as e:
            _log(f"POST {url} URLError: {e.reason}")
            raise IBroadcastError(f"Network error: {e.reason}") from e

    # ------------------------------------------------------------------
    # Authentication
    # ------------------------------------------------------------------

    def is_authenticated(self):
        return bool(self.token and self.user_id)

    def login(self, email, password):
        """Authenticate with email/password. Returns (success, message)."""
        data = {
            "mode": "status",
            "email_address": email,
            "password": password,
            "version": "1.0.0",
            "client": self.CLIENT,
            "supported_types": False,
        }
        try:
            resp = self._post(self.API_URL, data)
        except IBroadcastError as e:
            return False, str(e)

        if "user" not in resp:
            msg = resp.get("message") or str(resp)
            return False, msg

        self.user_id = resp["user"]["id"]
        self.token   = resp["user"]["token"]
        return True, "Login successful"

    # ------------------------------------------------------------------
    # Library
    # ------------------------------------------------------------------

    def load_library(self, force_refresh=False):
        """Fetch and cache the full library. Returns True on success."""
        cache_file = os.path.join(self.profile_path, "library_cache_v2.json")

        if not force_refresh and os.path.exists(cache_file):
            try:
                with open(cache_file) as f:
                    cached = json.load(f)
                # JSON serialises int keys as strings; convert them back
                self._library = {
                    section: {int(k): v for k, v in items.items()}
                    for section, items in cached["library"].items()
                }
                self._settings         = cached["settings"]
                self._streaming_server = self._settings.get("streaming_server")
                self._artwork_server   = self._settings.get("artwork_server")
                _log(f"Library loaded from cache: {len(self._library['tracks'])} tracks")
                return True
            except Exception as e:
                _log(f"Cache load failed: {e}")

        data = {
            "_token":         self.token,
            "_userid":        self.user_id,
            "client":         self.CLIENT,
            "version":        "1.0.0",
            "mode":           "library",
            "supported_types": False,
        }
        try:
            resp = self._post(self.LIBRARY_URL, data)
        except IBroadcastError:
            return False

        if "library" not in resp:
            return False

        raw      = resp["library"]
        settings = resp.get("settings", {})

        library = {
            "tracks":    self._parse_section(raw.get("tracks",    {}), "track_id"),
            "albums":    self._parse_section(raw.get("albums",    {}), "album_id"),
            "artists":   self._parse_section(raw.get("artists",   {}), "artist_id"),
            "playlists": self._parse_section(raw.get("playlists", {}), "playlist_id"),
        }

        self._library          = library
        self._settings         = settings
        self._streaming_server = settings.get("streaming_server")
        self._artwork_server   = settings.get("artwork_server")

        os.makedirs(self.profile_path, exist_ok=True)
        with open(cache_file, "w") as f:
            json.dump({"library": library, "settings": settings}, f)
        return True

    def _parse_section(self, data, id_key, filter_trashed=True):
        """Convert the array+map library section into a dict of named dicts.
        When filter_trashed is True, items with a truthy 'trashed' field are excluded."""
        if not isinstance(data, dict) or "map" not in data:
            return {}
        keymap = {idx: name for name, idx in data["map"].items()
                  if not isinstance(idx, dict)}
        result = {}
        for key, value in data.items():
            if isinstance(value, list):
                item = {keymap[i]: value[i] for i in range(len(value)) if i in keymap}
                if filter_trashed and item.get("trashed"):
                    continue
                item[id_key] = int(key)
                result[int(key)] = item
        return result

    # ------------------------------------------------------------------
    # Getters
    # ------------------------------------------------------------------

    def get_artists(self):
        """Return sorted list of album artists (artists that are primary artist on ≥1 album).

        iBroadcast creates artist records for every collaborator/producer on every track,
        resulting in thousands of noise entries with no albums.  We filter to only those
        artists whose id appears as artist_id on at least one album.
        """
        if not self._library:
            return []
        album_artist_ids = {
            alb.get("artist_id")
            for alb in self._library["albums"].values()
            if alb.get("artist_id") is not None
        }
        results = [
            {
                "id":         a["artist_id"],
                "name":       a.get("name") or f"Artist {a['artist_id']}",
                "artwork_id": a.get("artwork_id"),
            }
            for a in self._library["artists"].values()
            if a["artist_id"] in album_artist_ids
        ]
        return sorted(results, key=lambda x: x["name"].casefold())

    def get_albums(self, artist_id=None):
        """Return sorted list of album dicts, optionally filtered by artist."""
        if not self._library:
            return []

        # Build album_id → artwork_id, earliest uploaded_on, and total plays from tracks
        track_artwork = {}
        track_uploaded = {}
        track_plays = {}
        for trk in self._library["tracks"].values():
            aid = trk.get("album_id")
            if aid is not None:
                if aid not in track_artwork and trk.get("artwork_id"):
                    track_artwork[aid] = trk["artwork_id"]
                uon = f"{trk.get('uploaded_on', '')} {trk.get('uploaded_time', '')}".strip()
                if uon and (aid not in track_uploaded or uon < track_uploaded[aid]):
                    track_uploaded[aid] = uon
                track_plays[aid] = track_plays.get(aid, 0) + int(trk.get("plays") or 0)

        results = []
        for alb in self._library["albums"].values():
            alb_id = alb["album_id"]
            artwork_id = alb.get("artwork_id") or track_artwork.get(alb_id)
            results.append({
                "id":         alb_id,
                "name":       alb.get("name") or f"Album {alb_id}",
                "artist_id":  alb.get("artist_id"),
                "year":       alb.get("year", ""),
                "artwork_id": artwork_id,
                "rating":     alb.get("rating", 0),
                "plays":      track_plays.get(alb_id, 0),
                "uploaded_on": track_uploaded.get(alb_id, ""),
            })
        if artist_id:
            results = [a for a in results if str(a["artist_id"]) == str(artist_id)]
        return sorted(results, key=lambda x: x["name"].casefold())

    def get_tracks(self, album_id=None, artist_id=None, playlist_id=None):
        """Return sorted list of track dicts, optionally filtered.

        artist_id on each returned track is the album's primary artist_id, not the
        track-level artist_id (which can be a collaboration combo like
        "Flying Lotus, George Clinton").  The original track-level artist_id is
        preserved as track_artist_id so it can be displayed as additional credits.
        """
        if not self._library:
            return []

        # album_id → album's primary artist_id (used to override track-level artist_id)
        album_artist_map = {
            int(alb_id): alb.get("artist_id")
            for alb_id, alb in self._library["albums"].items()
            if isinstance(alb, dict)
        }

        playlist_ids = None
        if playlist_id:
            pl = self._library["playlists"].get(int(playlist_id))
            if pl:
                playlist_ids = {int(t) for t in (pl.get("tracks") or [])}
            else:
                return []

        results = []
        for trk in self._library["tracks"].values():
            tid        = trk["track_id"]
            trk_alb_id = trk.get("album_id")
            alb_artist = album_artist_map.get(int(trk_alb_id)) if trk_alb_id is not None else None
            # Primary artist for this track = album artist; fall back to track artist
            primary_artist_id = alb_artist or trk.get("artist_id")

            if playlist_ids is not None and tid not in playlist_ids:
                continue
            if album_id and str(trk_alb_id or "") != str(album_id):
                continue
            # Filter by artist uses the album artist so browsing by artist shows all
            # tracks from that artist's albums (even collab tracks)
            if artist_id and str(primary_artist_id or "") != str(artist_id):
                continue

            results.append({
                "id":              tid,
                "title":           trk.get("title") or f"Track {tid}",
                "album_id":        trk_alb_id,
                "artist_id":       primary_artist_id,   # album's primary artist
                "track_artist_id": trk.get("artist_id"), # original track-level artist (credits)
                "artwork_id":      trk.get("artwork_id"),
                "track_number":    int(trk.get("track") or 0),
                "year":            trk.get("year", ""),
                "duration":        int(trk.get("length") or 0),
                "genre":           trk.get("genre", "") or "",
                "file":            trk.get("file"),
                "rating":          trk.get("rating", 0),
                "plays":           trk.get("plays", 0),
                "uploaded_on":     f"{trk.get('uploaded_on', '')} {trk.get('uploaded_time', '')}".strip(),
            })
        return sorted(results, key=lambda x: (x["track_number"], x["title"].casefold()))

    def get_playlists(self):
        """Return sorted list of playlist dicts."""
        if not self._library:
            return []
        results = [
            {
                "id":          pl["playlist_id"],
                "name":        pl.get("name") or f"Playlist {pl['playlist_id']}",
                "description": pl.get("description", "") or "",
            }
            for pl in self._library["playlists"].values()
        ]
        return sorted(results, key=lambda x: x["name"].casefold())

    def get_artist_name(self, artist_id):
        if not artist_id or not self._library:
            return ""
        a = self._library["artists"].get(int(artist_id) if str(artist_id).isdigit() else -1, {})
        return a.get("name", "")

    def get_album_name(self, album_id):
        if not album_id or not self._library:
            return ""
        a = self._library["albums"].get(int(album_id) if str(album_id).isdigit() else -1, {})
        return a.get("name", "")

    # ------------------------------------------------------------------
    # Streaming & artwork
    # ------------------------------------------------------------------

    def get_stream_url(self, track_id, bitrate="128"):
        """
        Build the streaming URL for a track.

        Format (from official docs):
          [server]/[file]?Expires=[ms]&Signature=[token]&file_id=[id]
                         &user_id=[uid]&platform=[app]&version=[ver]

        The file field from the library already contains the default bitrate
        prefix (e.g. /128/d0c/6f4/21127414). Replace it with the desired one.
        Use 'orig' for original quality (no transcoding).
        """
        if not self._library:
            _log(f"get_stream_url({track_id}): library not loaded")
            return None
        tid = int(track_id) if str(track_id).isdigit() else -1
        trk = self._library["tracks"].get(tid)
        if not trk:
            _log(f"get_stream_url({track_id}): track id {tid} not found in library (track count={len(self._library['tracks'])})")
            return None
        file_path = trk.get("file")
        _log(f"get_stream_url({track_id}): file={file_path!r} streaming_server={self._streaming_server!r}")
        if not file_path:
            _log(f"get_stream_url({track_id}): track has no file field, track data={trk}")
            return None

        # Replace the leading bitrate segment: /128/... → /320/...
        file_path = re.sub(r"^/\d+/", f"/{bitrate}/", file_path)

        server  = self._streaming_server or "https://streaming.ibroadcast.com"
        expires = int(time.time() * 1000)  # current time in milliseconds

        params = urllib.parse.urlencode({
            "Expires":   expires,
            "Signature": self.token,
            "file_id":   track_id,
            "user_id":   self.user_id,
            "platform":  self.CLIENT,
            "version":   "1.0.0",
        })
        return f"{server}{file_path}?{params}"

    def get_artwork_url(self, artwork_id, size=1000):
        if not artwork_id:
            return None
        server = self._artwork_server or "https://artwork.ibroadcast.com"
        return f"{server}/artwork/{artwork_id}-{size}"

    # ------------------------------------------------------------------
    # Search
    # ------------------------------------------------------------------

    def search(self, query):
        if not self._library or not query:
            return []
        q = query.casefold()
        all_tracks = self.get_tracks()
        title_matches  = [t for t in all_tracks if q in t["title"].casefold()]
        seen = {t["id"] for t in title_matches}
        artist_matches = [
            t for t in all_tracks
            if t["id"] not in seen and q in self.get_artist_name(t["artist_id"]).casefold()
        ]
        return title_matches + artist_matches
