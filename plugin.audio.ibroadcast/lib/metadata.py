"""
Metadata enrichment using the same sources as the Kodi universal scrapers:
  1. TheAudioDB   — biography, images, genres, styles, moods (free, no key needed)
  2. FanArt.tv    — high-res fanart, clearlogo, clearart, banner
                    (free API key required; only used when key is configured)
  3. MusicBrainz  — fallback MBID source when TADB doesn't supply one and FTV is requested

TheAudioDB name-search is used as the primary lookup (no rate limit, no API key).
The MBID returned by TADB is reused for FanArt.tv, so MusicBrainz is only called
as a last resort for the FanArt.tv path.

Results are cached to disk for 30 days.  Bulk prefetch helpers are provided so
the refresh-library action can warm the cache for all artists and albums.
"""

import json
import os
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
MB_UA     = "iBroadcast-Kodi/1.2.19 (https://github.com/ChivanCOM/kodi-repository)"
TADB_BASE = "https://www.theaudiodb.com/api/v1/json/2"
FTV_BASE  = "https://webservice.fanart.tv/v3/music"
CACHE_TTL = 30 * 86400  # 30 days


class MetadataClient:
    def __init__(self, profile_path, fanart_api_key=""):
        self._dir = os.path.join(profile_path, "metadata_cache")
        os.makedirs(self._dir, exist_ok=True)
        self._ftv_key = fanart_api_key

    # ── cache ───────────────────────────────────────────────────────────────

    def _ck(self, prefix, item_id):
        """Cache filename keyed by iBroadcast integer ID — safe for any artist/album name."""
        return f"{prefix}_{item_id}.json"

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

    # ── TheAudioDB (primary, no rate limit) ─────────────────────────────────

    def _tadb_search_artist(self, name):
        r = self._get(f"{TADB_BASE}/search.php?s={urllib.parse.quote(name)}")
        if r and r.get("artists"):
            return r["artists"][0]
        return None

    def _tadb_search_album(self, artist_name, album_name):
        url = (f"{TADB_BASE}/searchalbum.php"
               f"?s={urllib.parse.quote(artist_name)}"
               f"&a={urllib.parse.quote(album_name)}")
        r = self._get(url)
        if r and r.get("album"):
            return r["album"][0]
        return None

    # ── MusicBrainz (only needed when TADB has no MBID and FTV is requested) ──

    def _mb_artist_mbid(self, name):
        q = f'artist:"{urllib.parse.quote(name)}"'
        r = self._get(f"{MB_BASE}/artist/?fmt=json&query={q}&limit=5", MB_UA)
        if not r:
            return None
        for a in (r.get("artists") or []):
            if (a.get("name") or "").lower() == name.lower():
                return a["id"]
        artists = r.get("artists") or []
        return artists[0]["id"] if artists else None

    def _mb_release_mbids(self, artist_name, album_name):
        """Return (release_mbid, release_group_mbid, artist_mbid) for FTV album art."""
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

    # ── FanArt.tv ───────────────────────────────────────────────────────────

    def _ftv_by_mbid(self, artist_mbid):
        if not self._ftv_key or not artist_mbid:
            return None
        return self._get(f"{FTV_BASE}/{artist_mbid}?api_key={self._ftv_key}")

    # ── internal ────────────────────────────────────────────────────────────

    @staticmethod
    def _first(lst):
        return lst[0]["url"] if lst else ""

    def _apply_ftv_artist(self, ftv, d):
        """Overlay FanArt.tv artist images onto result dict d (higher quality)."""
        if not ftv:
            return
        if ftv.get("artistbackground"):
            bg = ftv["artistbackground"]
            d["fanart"]  = bg[0]["url"] if len(bg) > 0 else d.get("fanart", "")
            d["fanart2"] = bg[1]["url"] if len(bg) > 1 else ""
            d["fanart3"] = bg[2]["url"] if len(bg) > 2 else ""
            d["fanart4"] = bg[3]["url"] if len(bg) > 3 else ""
        if ftv.get("artistthumb"):
            d["thumb"]     = self._first(ftv["artistthumb"])
            d["widethumb"] = self._first(ftv["artistthumb"])  # FTV thumb is wide-format
        if ftv.get("hdmusiclogo"):  d["clearlogo"] = self._first(ftv["hdmusiclogo"])
        elif ftv.get("musiclogo"):  d["clearlogo"] = self._first(ftv["musiclogo"])
        if ftv.get("hdmusicart"):   d["clearart"]  = self._first(ftv["hdmusicart"])
        elif ftv.get("musicart"):   d["clearart"]  = self._first(ftv["musicart"])
        if ftv.get("musicbanner"):  d["banner"]    = self._first(ftv["musicbanner"])

    # ── public: artist ──────────────────────────────────────────────────────

    def get_artist_info(self, artist_id, name, force=False):
        """Fetch and cache artist metadata.  Primary: TADB name-search.  Optional: FTV."""
        if not name:
            return {}
        k = self._ck("ar", artist_id)
        if not force:
            cached = self._load(k)
            if cached is not None:
                return cached

        _log(f"artist {artist_id}: {name}")
        a = self._tadb_search_artist(name)

        if a:
            d = {
                "mbid":       a.get("strMusicBrainzID") or "",
                "biography":  a.get("strBiographyEN") or "",
                "genre":      a.get("strGenre") or "",
                "style":      a.get("strStyle") or "",
                "mood":       a.get("strMood") or "",
                "country":    a.get("strCountry") or "",
                "born_year":  a.get("intFormedYear") or a.get("intBornYear") or "",
                "thumb":      a.get("strArtistThumb") or "",
                "widethumb":  a.get("strArtistWideThumb") or "",   # 16:9 wide thumb → landscape
                "fanart":     a.get("strArtistFanart") or "",
                "fanart2":    a.get("strArtistFanart2") or "",
                "fanart3":    a.get("strArtistFanart3") or "",
                "fanart4":    a.get("strArtistFanart4") or "",
                "banner":     a.get("strArtistBanner") or "",
                "clearlogo":  a.get("strArtistLogo") or "",
                "clearart":   a.get("strArtistClearArt") or "",
                "cutout":     a.get("strArtistCutout") or "",
            }
        else:
            d = {}

        # FanArt.tv — higher quality, overrides TADB images; also sole source when TADB has nothing
        if self._ftv_key:
            mbid = d.get("mbid") or self._mb_artist_mbid(name)
            if mbid:
                d["mbid"] = mbid
                self._apply_ftv_artist(self._ftv_by_mbid(mbid), d)

        d["_ftv_checked"] = bool(self._ftv_key)
        self._save(k, d)
        return d

    def get_artist_info_cached(self, artist_id):
        """Return cached artist info without any HTTP calls."""
        if not artist_id:
            return {}
        return self._load(self._ck("ar", artist_id)) or {}

    # ── public: album ───────────────────────────────────────────────────────

    def get_album_info(self, album_id, artist_name, album_name, force=False):
        """Fetch and cache album metadata.  Primary: TADB name-search.  Optional: FTV."""
        if not artist_name or not album_name:
            return {}
        k = self._ck("al", album_id)
        if not force:
            cached = self._load(k)
            if cached is not None:
                return cached

        _log(f"album {album_id}: {artist_name} / {album_name}")
        alb = self._tadb_search_album(artist_name, album_name)

        if alb:
            d = {
                "description": alb.get("strDescriptionEN") or "",
                "genre":       alb.get("strGenre") or "",
                "style":       alb.get("strStyle") or "",
                "mood":        alb.get("strMood") or "",
                "theme":       alb.get("strTheme") or "",
                "speed":       alb.get("strSpeed") or "",
                "year":        alb.get("intYearReleased") or "",
                "rating":      alb.get("intScore") or "",
                "thumb":       alb.get("strAlbumThumbHQ") or alb.get("strAlbumThumb") or "",
                "thumb3d":     alb.get("strAlbum3DThumb") or alb.get("strAlbum3DCase") or "",
                "discart":     alb.get("strAlbumCDart") or "",
                "back":        alb.get("strAlbumBack") or "",
                "spine":       alb.get("strAlbumSpine") or "",
                "fanart":      "",
                # TADB directly supplies MBIDs — saves a MusicBrainz round-trip
                "mbid":        alb.get("strMusicBrainzID") or "",
                "artist_mbid": alb.get("strMusicBrainzArtistID") or "",
            }
        else:
            d = {}

        # FanArt.tv album art — use TADB MBIDs when available, fall back to MB query
        if self._ftv_key:
            rg_mbid    = d.get("mbid") or None
            artist_mbid = d.get("artist_mbid") or None
            if not (rg_mbid and artist_mbid):
                # TADB didn't supply both MBIDs — ask MusicBrainz
                _, mb_rg, mb_ar = self._mb_release_mbids(artist_name, album_name)
                rg_mbid    = rg_mbid    or mb_rg
                artist_mbid = artist_mbid or mb_ar
            if rg_mbid:
                d["mbid"] = rg_mbid
            if artist_mbid and rg_mbid:
                ftv = self._ftv_by_mbid(artist_mbid)
                if ftv:
                    alb_art = (ftv.get("albums") or {}).get(rg_mbid, {})
                    if alb_art.get("albumcover"): d["thumb"]   = self._first(alb_art["albumcover"])
                    if alb_art.get("cdart"):      d["discart"] = self._first(alb_art["cdart"])
                    if ftv.get("artistbackground"):
                        d["fanart"] = ftv["artistbackground"][0]["url"]

        d["_ftv_checked"] = bool(self._ftv_key)
        self._save(k, d)
        return d

    def get_album_info_cached(self, album_id):
        """Return cached album info without any HTTP calls."""
        if not album_id:
            return {}
        return self._load(self._ck("al", album_id)) or {}

    # ── bulk prefetch ────────────────────────────────────────────────────────

    def _needs_fetch(self, prefix, item_id):
        """True when cache entry is absent, expired, or was cached without FTV but key is now set."""
        d = self._load(self._ck(prefix, item_id))
        if d is None:
            return True
        # Re-fetch if a FTV key is configured now but wasn't used when this entry was written
        if self._ftv_key and not d.get("_ftv_checked"):
            return True
        return False

    def prefetch_artists(self, artists, on_progress=None, is_cancelled=None, force=False):
        """
        Pre-warm the cache for a list of (artist_id, name) tuples.
        force=False  — skip artists already in cache (subsequent refreshes are fast).
        force=True   — re-fetch every artist regardless of cache (full rebuild).
        on_progress(i, total, name) — called only for items being fetched.
        is_cancelled() — return True to abort.
        Returns (fetched, skipped) counts.
        """
        pending = artists if force else [(aid, n) for aid, n in artists if self._needs_fetch("ar", aid)]
        total   = len(pending)
        for i, (artist_id, name) in enumerate(pending):
            if is_cancelled and is_cancelled():
                break
            if on_progress:
                on_progress(i, total, name)
            self.get_artist_info(artist_id, name, force=force)
        return total, len(artists) - total

    def prefetch_albums(self, albums, on_progress=None, is_cancelled=None, force=False):
        """
        Pre-warm the cache for a list of (album_id, artist_name, album_name) tuples.
        force=True re-fetches every album regardless of cache.
        Returns (fetched, skipped) counts.
        """
        pending = (albums if force
                   else [(aid, ar, al) for aid, ar, al in albums if self._needs_fetch("al", aid)])
        total   = len(pending)
        for i, (album_id, artist_name, album_name) in enumerate(pending):
            if is_cancelled and is_cancelled():
                break
            if on_progress:
                on_progress(i, total, album_name)
            self.get_album_info(album_id, artist_name, album_name, force=force)
        return total, len(albums) - total
