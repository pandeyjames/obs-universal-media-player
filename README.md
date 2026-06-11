# OBS Universal Media Player Plugin

A native OBS source plugin that adds **Universal Media Player** directly to the
Add Source menu.

It resolves YouTube, Twitch, Vimeo, Dailymotion and other supported website
links with bundled `yt-dlp` and Streamlink, then plays the resolved stream
through a private native OBS Media Source. Direct media, HLS, RTMP, RTSP and
SRT URLs can bypass resolution.

## Supported links

Choose a provider or leave **Auto detect** selected:

- YouTube videos, Shorts, live streams, and playlist video links
- Twitch channels, live streams, VODs, and clips
- Vimeo and Dailymotion videos
- Facebook videos and reels
- Kick channels, live streams, VODs, and clips
- TikTok videos
- X/Twitter posts containing video
- Instagram posts and reels
- Streamable, Rumble, Reddit, and Bilibili videos
- SoundCloud audio
- Other sites supported by bundled `yt-dlp` or Streamlink
- Direct `.mp4`, `.webm`, `.m3u8`, RTMP, RTSP, and SRT URLs

The provider dropdown displays link guidance. **Best compatible** is the
recommended quality setting. If a selected resolution is unavailable, the
plugin automatically retries the best format offered by the service.

## Resolver engines

- **Auto** prefers Streamlink for Twitch and Kick live URLs and yt-dlp for
  general videos and VODs. If the preferred resolver fails, it tries the other.
- **yt-dlp** uses only the bundled yt-dlp executable.
- **Streamlink** uses only the bundled portable Streamlink runtime.
- **Direct URL** sends the URL directly to OBS without resolving it.

Streamlink is particularly useful for live channels. yt-dlp provides broader
website, VOD, clip, and audio support. Source information shows which resolver
actually succeeded when Auto mode is selected.

## Architecture

Universal Media Player is a native OBS input source implemented in C. It does
not embed a browser and it does not ask Streamlink or yt-dlp to play media.
Those tools are used only to resolve website URLs into direct playable stream
URLs.

```text
OBS Add Source
  -> universal_media_player source
      -> resolver engine
          -> direct URL bypass
          -> yt-dlp --get-url
          -> Streamlink --stream-url
      -> private OBS ffmpeg_source
          -> OBS video renderer
          -> OBS audio mixer
      -> scene item transform controls
          -> fit / stretch / center / reset on the active scene
```

### Source Layer

The plugin registers one OBS source type: `universal_media_player`. Each source
instance owns a private `ffmpeg_source` child. The child is not shown as a
separate user source; it is used internally so OBS still handles decoding,
network buffering, hardware decoding, audio mixing, media controls, and
rendering through its native media pipeline.

### Resolver Layer

The resolver layer runs external tools as short-lived child processes:

- `yt-dlp.exe --get-url` for broad website, VOD, clip, and audio support
- `streamlink.exe --stream-url` for live-stream focused providers
- direct passthrough for already-playable media or network stream URLs

Auto mode chooses a preferred resolver based on the provider and URL, then tries
the other resolver if the first one fails. The resolved URL is passed to the
private OBS Media Source as its input.

### Bundled Dependencies

Windows releases bundle pinned versions of yt-dlp and the official portable
Streamlink runtime. The Git repository does not commit generated third-party
executables. Instead, `scripts/fetch-windows-dependencies.ps1` downloads the
pinned artifacts and verifies SHA-256 hashes before placing them under
`data/bin`.

### Playback And Transforms

Playback is always handled by OBS, not by yt-dlp or Streamlink. The source
implements OBS media-control callbacks and forwards play, pause, restart, stop,
duration, and seek operations to the private media child.

Canvas scaling is applied through OBS scene-item transforms on the active scene:

- Fit uses `OBS_BOUNDS_SCALE_INNER`
- Stretch uses `OBS_BOUNDS_STRETCH`
- Center updates the item position and alignment
- Reset clears bounds, scale, rotation, and crop

This keeps source sizing compatible with OBS's own Transform menu behavior.

## Install

1. Close OBS.
2. Run `install.ps1` from the release package.
3. Start OBS.
4. Select **Sources > Add > Universal Media Player**.

No script loading or separate installation of `yt-dlp`, Streamlink, Python,
VLC, or another media player is required.

## Building from source

The repository does not commit generated third-party executables. On Windows,
fetch the pinned, checksum-verified dependencies before configuring CMake:

```powershell
.\scripts\fetch-windows-dependencies.ps1
cmake --preset windows-x64
cmake --build --preset windows-x64 --config RelWithDebInfo
```

Published Windows release ZIPs already contain these dependencies.

The plugin selects a single stream containing both video and audio because one
native OBS Media Source accepts one input URL. Some services, especially
YouTube, offer their highest resolutions only as separate video and audio
streams. In that case the plugin falls back to the best compatible combined
stream so preview and playback remain reliable.

## Source information and canvas scaling

The properties window shows the provider, media dimensions, OBS base canvas
dimensions, playback state, and duration. Use **Refresh Source Information**
after a stream finishes opening to update the displayed values.

The **Preview canvas scaling** menu provides:

- Fit inside the canvas while preserving aspect ratio
- Stretch to fill the canvas
- Center at the current size
- Reset transform

**Apply Scaling to Current Scene** changes this source in the active scene.
Enable **Automatically fit to canvas after resolving** to fit streams after
they are resolved or refreshed.

## Live streams

For Twitch and other live HLS sources, keep these options disabled:

- Restart playback when source becomes active
- Close stream when inactive
- Clear frame when playback ends

Use **Resolve / Refresh Stream** when a signed stream URL expires.

## Limitations

The plugin does not bypass DRM, subscriptions, geographic restrictions, or
provider access controls. Browser cookies can be selected for content that
requires an authenticated session when using yt-dlp. See
`THIRD-PARTY-NOTICES.md` for bundled component versions and licenses.
