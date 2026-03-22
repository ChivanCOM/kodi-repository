"""
Metadata enrichment using the same sources as the Kodi universal scrapers:
  1. MusicBrainz  — MBID lookup (free, no key)
  2. TheAudioDB   — biography, images, genres, styles, moods (free, no key)
  3. FanArt.tv    — high-res fanart, clearlogo, clearart, banner
                    (free API key required; only used when key is configured)

No dependency on metadata.artists.universal or metadata.album.universal being installed.
Results are cached to disk for 30 days so repeat views are instant.
"""

import json
import os
import hashlib
import time
import urllib.request
import urllib.parse

try:
    import xbmc
    def _log(msg):
        xbmc.log(f"[iBroadcast/meta] {msg}", xbmc.LOGINFO)
except ImportError:
    def _log(msg):
        print(f"[meta] {msg}")

MB_BASE   = "https://musicbrainz.org/ws/2"
MB_UA     = "iBroadcast-Kodi/1.2.11 (https://github.com/ChivanCOM/kodi-repository)"
TADB_BASE = "https://www.theaudiodb.com/api/v1/json/2"
FTV_BASE  = "https://webservice.fanart.tv/v3/music"
CACHE_TTL = 30 * 86400  # 30 days


class MetadataClient:
    def __init__(self, profile_path, fanart_api_key=""):
        self._dir = os.path.join(profile_path, "metadata_cache")
        os.makedirs(self._dir, exist_ok=True)
        self._ftv_key = fanart_api_key

    # ── cache ───────────────────────────────────────────────────────────────

    def _ck(self, *parts):
        raw = "|".join(str(p).strip().lower() for p in parts)
        return hashlib.md5(raw.encode()).hexdigest() + ".json"

    def _load(self, k):
        path = os.path.join(self._dir, k)
        if not os.path.exists(path):
            return None
        try:
            with open(path) as f:
                d = json.load(f)
            return None if time.time() - d.get("_t", 0) > CACHE_TTL else d
        except Exception:
            return None

    def _save(self, k, d):
        d["_t"] = time.time()
        try:
            with open(os.path.join(self._dir, k), "w") as f:
                json.dump(d, f)
        except Exception:
            pass

    # ── HTTP ────────────────────────────────────────────────────────────────

    def _get(self, url, ua=None):
        try:
            req = urllib.request.Request(url, headers={
                "User-Agent": ua or "Mozilla/5.0 (compatible; Kodi/iBroadcast-Plugin/1.0)",
                "Accept": "application/json",
            })
            with urllib.request.urlopen(req, timeout=10) as r:
                return json.loads(r.read().decode("utf-8"))
        except Exception as e:
            _log(f"GET {url}: {e}")
            return None

    # ── MusicBrainz ─────────────────────────────────────────────────────────

    def _mb_artist(self, name):
        """Return best-match artist MBID."""
        q = f'artist:"{urllib.parse.quote(name)}"'
        r = self._get(f"{MB_BASE}/artist/?fmt=json&query={q}&limit=5", MB_UA)
        if not r:
            return None
        for a in (r.get("artists") or []):
            if (a.get("name") or "").lower() == name.lower():
                return a["id"]
        artists = r.get("artists") or []
        return artists[0]["id"] if artists else None

    def _mb_release(self, artist_name, album_name):
        """Return (release_mbid, release_group_mbid, artist_mbid)."""
        q = (f'release:"{urllib.parse.quote(album_name)}" AND '
             f'artistname:"{urllib.parse.quote(artist_name)}"')
        r = self._get(f"{MB_BASE}/release/?fmt=json&query={q}&limit=5", MB_UA)
        if not r:
            return None, None, None
        releases = [rel for rel in (r.get("releases") or [])
                    if (rel.get("status") or "").lower() == "official"]
        if not releases:
            releases = r.get("releases") or []
        if not releases:
            return None, None, None
        rel = releases[0]
        artist_mbid = None
        credits = rel.get("artist-credit") or []
        if credits and isinstance(credits[0], dict):
            artist_mbid = (credits[0].get("artist") or {}).get("id")
        return rel.get("id"), (rel.get("release-group") or {}).get("id"), artist_mbid

    # ── TheAudioDB ──────────────────────────────────────────────────────────

    def _tadb_artist(self, mbid):
        r = self._get(f"{TADB_BASE}/artist-mb.php?i={mbid}")
        if r and r.get("artists"):
            return r["artists"][0]
        return None

    def _tadb_album(self, release_mbid):
        r = self._get(f"{TADB_BASE}/album-mb.php?i={release_mbid}")
        if r and r.get("album"):
            return r["album"][0]
        return None

    # ── FanArt.tv ───────────────────────────────────────────────────────────

    def _ftv_artist(self, artist_mbid):
        """Fetch FanArt.tv data for an artist. Also contains album art keyed by release-group MBID."""
        if not self._ftv_key:
            return None
        return self._get(f"{FTV_BASE}/{artist_mbid}?api_key={self._ftv_key}")

    # ── internal helper ─────────────────────────────────────────────────────

    @staticmethod
    def _first(lst):
        return lst[0]["url"] if lst else ""

    # ── public: artist ──────────────────────────────────────────────────────

    def get_artist_info(self, name):
        """Fetch and cache full artist metadata from MusicBrainz + TheAudioDB + FanArt.tv."""
        if not name:
            return {}
        k = self._ck("ar", name)
        cached = self._load(k)
        if cached is not None:
            return cached

        _log(f"artist lookup: {name}")

        mbid = self._mb_artist(name)
        if not mbid:
            self._save(k, {})
            return {}

        d = {
            "mbid": mbid, "biography": "", "genre": "", "style": "", "mood": "",
            "country": "", "born_year": "", "thumb": "", "fanart": "",
            "fanart2": "", "fanart3": "", "banner": "", "clearlogo": "", "clearart": "",
        }

        # TheAudioDB
        a = self._tadb_artist(mbid)
        if a:
            d.update({
                "biography": a.get("strBiographyEN") or "",
                "genre":     a.get("strGenre") or "",
                "style":     a.get("strStyle") or "",
                "mood":      a.get("strMood") or "",
                "country":   a.get("strCountry") or "",
                "born_year": a.get("intFormedYear") or a.get("intBornYear") or "",
                "thumb":     a.get("strArtistThumb") or "",
                "fanart":    a.get("strArtistFanart") or "",
                "fanart2":   a.get("strArtistFanart2") or "",
                "fanart3":   a.get("strArtistFanart3") or "",
                "banner":    a.get("strArtistBanner") or "",
                "clearlogo": a.get("strArtistLogo") or "",
                "clearart":  a.get("strArtistClearArt") or "",
            })

        # FanArt.tv — higher quality, overrides TADB images
        ftv = self._ftv_artist(mbid)
        if ftv:
            if ftv.get("artistbackground"):
                bg = ftv["artistbackground"]
                d["fanart"]  = bg[0]["url"] if len(bg) > 0 else d["fanart"]
                d["fanart2"] = bg[1]["url"] if len(bg) > 1 else ""
                d["fanart3"] = bg[2]["url"] if len(bg) > 2 else ""
            if ftv.get("artistthumb"):  d["thumb"]    = self._first(ftv["artistthumb"])
            if ftv.get("hdmusiclogo"):  d["clearlogo"] = self._first(ftv["hdmusiclogo"])
            elif ftv.get("musiclogo"):  d["clearlogo"] = self._first(ftv["musiclogo"])
            if ftv.get("hdmusicart"):   d["clearart"]  = self._first(ftv["hdmusicart"])
            elif ftv.get("musicart"):   d["clearart"]  = self._first(ftv["musicart"])
            if ftv.get("musicbanner"):  d["banner"]    = self._first(ftv["musicbanner"])

        self._save(k, d)
        return d

    def get_artist_info_cached(self, name):
        """Return cached artist info without any HTTP calls (for non-blocking list views)."""
        if not name:
            return {}
        return self._load(self._ck("ar", name)) or {}

    # ── public: album ───────────────────────────────────────────────────────

    def get_album_info(self, artist_name, album_name):
        """Fetch and cache full album metadata from MusicBrainz + TheAudioDB + FanArt.tv."""
        if not artist_name or not album_name:
            return {}
        k = self._ck("al", artist_name, album_name)
        cached = self._load(k)
        if cached is not None:
            return cached

        _log(f"album lookup: {artist_name} / {album_name}")

        release_mbid, rg_mbid, artist_mbid = self._mb_release(artist_name, album_name)
        if not release_mbid:
            self._save(k, {})
            return {}

        d = {
            "mbid": release_mbid, "description": "", "genre": "", "style": "",
            "mood": "", "theme": "", "rating": "", "year": "",
            "thumb": "", "fanart": "", "discart": "", "back": "",
        }

        # TheAudioDB
        alb = self._tadb_album(release_mbid)
        if alb:
            d.update({
                "description": alb.get("strDescriptionEN") or "",
                "genre":       alb.get("strGenre") or "",
                "style":       alb.get("strStyle") or "",
                "mood":        alb.get("strMood") or "",
                "theme":       alb.get("strTheme") or "",
                "year":        alb.get("intYearReleased") or "",
                "rating":      alb.get("intScore") or "",
                "thumb":       alb.get("strAlbumThumbHQ") or alb.get("strAlbumThumb") or "",
                "discart":     alb.get("strAlbumCDart") or "",
                "back":        alb.get("strAlbumBack") or "",
            })

        # FanArt.tv — artist endpoint holds album art keyed by release-group MBID
        if artist_mbid and rg_mbid and self._ftv_key:
            ftv = self._ftv_artist(artist_mbid)
            if ftv:
                alb_art = (ftv.get("albums") or {}).get(rg_mbid, {})
                if alb_art.get("albumcover"): d["thumb"]   = self._first(alb_art["albumcover"])
                if alb_art.get("cdart"):      d["discart"] = self._first(alb_art["cdart"])
                if not d["fanart"] and ftv.get("artistbackground"):
                    d["fanart"] = ftv["artistbackground"][0]["url"]

        self._save(k, d)
        return d
