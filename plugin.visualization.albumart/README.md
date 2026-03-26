# ChivanCOM Album Art Visualizer for Kodi

A music visualizer for Kodi that puts your album art front and centre, surrounded by an animated background that reacts to the music you're playing.

## What it looks like

When a track is playing, the screen is divided into two zones:

**Left — Album art**
The current track's album art is displayed on the left side of the screen, tall and prominent. It has a subtle coloured border whose hue is extracted from the art itself, so it always feels cohesive with the cover.

**Right — Track information**
To the right of the art, the track title, artist, and album name are displayed in clean, proportionally spaced text. Font sizes and spacing follow a layout derived from the cover art proportions so everything feels balanced at any screen resolution.

**Background**
Behind everything, a full-screen animated shader fills the canvas. The background colours are also derived from the album art's palette — a highlight colour and a darker complementary tone — so the entire screen feels like a single, unified visual tied to what's playing. The background is blurred and darkened slightly so it never competes with the art and text in the foreground.

## Background shaders

You can choose from several animated backgrounds in the add-on settings:

| Shader | Description |
|--------|-------------|
| Audio Visualizer | Flame and wave forms that react to the audio spectrum |
| Metaballs | Smooth, organic blobs that drift and merge |
| Nebula Ring | A rotating cosmic ring with volumetric noise |
| Wavy Lines | Flowing lines across the screen |
| Tunnelwisp | A spiralling tunnel of light |
| Plasma | Classic plasma colour field |
| Spectrum | Audio spectrum bars |
| Metaballs Chroma | Metaballs using the album's extracted palette colours *(default)* |
| None | Solid black background |

## Settings

| Setting | Description |
|---------|-------------|
| Background Shader | Choose which animated background to use |
| Enable Background Blur | Blur the background render for a softer, more cinematic look |

## Installation

Install via the ChivanCOM Repository. Follow the step-by-step guide at:

**[https://github.com/ChivanCOM/kodi-repository-install](https://github.com/ChivanCOM/kodi-repository-install)**

## Support

For questions or bug reports, please open an issue at [github.com/ChivanCOM/kodi-repository/issues](https://github.com/ChivanCOM/kodi-repository/issues).

## Credits

This add-on was co-developed with the assistance of Claude AI.
