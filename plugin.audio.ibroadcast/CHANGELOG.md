# Changelog

## 1.3.1

### Fixed
- Date Added sorting now works correctly for albums and tracks
- Combines `uploaded_on` (date) and `uploaded_time` (time) into Kodi's expected format
- Albums derive their date from the earliest track since the iBroadcast API does not include upload dates on albums

## 1.3.0

### Added
- Original quality streaming option (requires active iBroadcast subscription)
- Sort albums and tracks by Date Added, Rating, or Play Count
- Trashed items are now automatically filtered from the library
- README with full feature list, installation guide, and credits

### Changed
- Album play count is calculated as the sum of all track plays
- Album "date added" falls back to the earliest track upload date when not set on the album itself
- Updated addon description with expanded feature overview

## 1.2.25

### Fixed
- Background metadata prefetch no longer crashes Kodi when the plugin context is cleaned up
- All Kodi addon API values are now pre-resolved in the main thread before background work starts

## 1.2.0

- Initial public release
- Browse by artist, album, playlist, or search
- Adjustable bitrate (96–320 kbps)
- Artist and album metadata via MusicBrainz, TheAudioDB, Discogs, and iBroadcast
- FanArt.tv integration for high-resolution artwork
- Background metadata prefetch with progress notifications
