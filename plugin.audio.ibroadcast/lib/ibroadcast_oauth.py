"""
iBroadcast OAuth 2.0 device-code helpers.

Stdlib-only and Kodi-agnostic so it stays unit-testable. The driver in
default.py wires this to xbmcgui dialogs and persists tokens to addon
settings.

iBroadcast deprecated email/password auth on 2025-10-21 and disabled it
on 2025-12-31. See https://help.ibroadcast.com/developer/authentication

The device-code grant is the right fit for Kodi: many installs run on
TV / headless boxes with no usable browser+redirect, and the user can
authorize from a phone instead.
"""

import json
import time
import urllib.error
import urllib.parse
import urllib.request


# Registered at https://media.ibroadcast.com → Apps → Developers.
# This is the public client_id; the matching client_secret is intentionally
# NOT shipped — the device-code flow does not require it, and a public
# Kodi addon source cannot keep a secret.
CLIENT_ID = "1c01a48744ce11f19610b49691aa2236"

OAUTH_BASE = "https://oauth.ibroadcast.com"

# Read-only scopes only. Add :write scopes if the addon ever needs to
# edit playlists, ratings, or upload — users will be re-prompted.
SCOPES = "user.library:read user.queue:read"


class OAuthError(Exception):
    """Network or terminal OAuth failure."""


# Result codes returned by exchange_device_code()
EXCHANGE_OK        = "ok"
EXCHANGE_PENDING   = "pending"
EXCHANGE_SLOW_DOWN = "slow_down"
EXCHANGE_ERROR     = "error"


def request_device_code():
    """Start the device-code flow.

    Returns the server response dict with keys: device_code, user_code,
    verification_uri, verification_uri_complete, interval, expires_in.
    """
    status, data = _post_form("/device/code", {
        "client_id": CLIENT_ID,
        "scope":     SCOPES,
    })
    if status >= 400:
        raise OAuthError(_describe(data, status))
    for key in ("device_code", "user_code", "verification_uri", "interval", "expires_in"):
        if key not in data:
            raise OAuthError(f"OAuth /device/code response missing '{key}'")
    return data


def exchange_device_code(device_code):
    """Single attempt at exchanging a device_code for tokens.

    Returns (code, payload):
      EXCHANGE_OK        → payload is a finalized token dict (with expires_at)
      EXCHANGE_PENDING   → payload is None; keep polling at the current interval
      EXCHANGE_SLOW_DOWN → payload is None; keep polling, raise interval
      EXCHANGE_ERROR     → payload is a human-readable error string
    """
    status, data = _post_form("/token", {
        "grant_type":  "device_code",
        "client_id":   CLIENT_ID,
        "device_code": device_code,
    })
    if status == 200 and isinstance(data, dict) and "access_token" in data:
        return EXCHANGE_OK, _finalize_token(data)

    err = (data or {}).get("error") if isinstance(data, dict) else None
    if err == "authorization_pending":
        return EXCHANGE_PENDING, None
    if err == "slow_down":
        return EXCHANGE_SLOW_DOWN, None
    return EXCHANGE_ERROR, _describe(data, status)


def refresh(refresh_token):
    """Exchange a refresh_token for fresh tokens. Returns finalized token dict."""
    status, data = _post_form("/token", {
        "grant_type":    "refresh_token",
        "client_id":     CLIENT_ID,
        "refresh_token": refresh_token,
    })
    if status >= 400 or not isinstance(data, dict) or "access_token" not in data:
        raise OAuthError(_describe(data, status))
    return _finalize_token(data)


def revoke(refresh_token):
    """Best-effort token revoke. Returns True on success."""
    try:
        status, _ = _post_form("/revoke", {
            "client_id":     CLIENT_ID,
            "refresh_token": refresh_token,
        })
    except OAuthError:
        return False
    return status < 400


def is_expired(expires_at, skew=120):
    """True if the token is within `skew` seconds of expiry (or invalid)."""
    if not expires_at:
        return True
    try:
        return int(time.time()) >= int(expires_at) - skew
    except (TypeError, ValueError):
        return True


# ----------------------------------------------------------------------
# Internal
# ----------------------------------------------------------------------

def _post_form(path, form):
    body = urllib.parse.urlencode(form).encode("utf-8")
    req = urllib.request.Request(
        OAUTH_BASE + path,
        data=body,
        headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "Accept":       "application/json",
            "User-Agent":   "Kodi-iBroadcast/1.4",
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return resp.status, _safe_json(resp.read())
    except urllib.error.HTTPError as e:
        return e.code, _safe_json(e.read())
    except urllib.error.URLError as e:
        raise OAuthError(f"Network error: {e.reason}") from e


def _safe_json(raw):
    try:
        return json.loads(raw.decode("utf-8"))
    except Exception:
        return {}


def _finalize_token(data):
    data = dict(data)
    expires_in = int(data.get("expires_in") or 0)
    data["expires_at"] = int(time.time()) + max(expires_in, 0)
    return data


def _describe(data, status):
    if isinstance(data, dict):
        err  = data.get("error") or f"http_{status}"
        desc = data.get("error_description") or ""
        return f"{err}: {desc}".strip(": ")
    return f"http_{status}"
