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

## Playback modes

- **Stream immediately** resolves a URL and starts playback as soon as OBS can
  open the stream. This is best for live streams and quick playback.
- **Download/remux high quality** uses yt-dlp to download the best matching
  video track and best audio track, merges them with bundled FFmpeg, then plays
  the resulting local file through the same OBS source. This is best for normal
  videos, VODs, and clips where 1080p or higher is split into separate video and
  audio streams.

Download/remux mode still appears as one OBS source. The separate video and
audio tracks exist only while yt-dlp and FFmpeg are preparing the file. Once
complete, OBS receives one local media file with video and audio together, so
media controls, audio mixing, transforms, and source visibility stay unified.

The preview can stay blank while download/remux is running. For long VODs, this
can take a long time because the entire video must be downloaded and merged
before OBS can open the completed local file.

Resolver and remux processes started from the bundled `yt-dlp`, Streamlink, and
FFmpeg runtime are attached to the OBS process on Windows. If OBS exits or is
terminated, Windows also terminates that bundled process tree. On the next
plugin load, the plugin cleans up any stale bundled resolver processes left by
older builds.

Do not use download/remux mode for live streams. A live stream has no fixed end,
so the merged file cannot be completed before playback. Use Streamlink or Direct
URL mode for live HLS/RTMP/SRT sources.

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
      -> playback mode
          -> stream immediately
              -> direct URL bypass
              -> yt-dlp --get-url
              -> Streamlink --stream-url
          -> download/remux high quality
              -> yt-dlp bestvideo+bestaudio
              -> bundled FFmpeg merge/remux
              -> local cache file
      -> private OBS ffmpeg_source
          -> OBS video renderer
          -> OBS audio mixer
      -> scene item transform controls
          -> fit / stretch / center / reset on the active scene
      -> bundled process cleanup
          -> Windows job object terminates yt-dlp, Streamlink, and FFmpeg with OBS
```

### Source Layer

The plugin registers one OBS source type: `universal_media_player`. Each source
instance owns a private `ffmpeg_source` child. The child is not shown as a
separate user source; it is used internally so OBS still handles decoding,
network buffering, hardware decoding, audio mixing, media controls, and
rendering through its native media pipeline.

### Resolver Layer

The resolver layer runs external tools as short-lived child processes.

Immediate streaming uses:

- `yt-dlp.exe --get-url` for broad website, VOD, clip, and audio support
- `streamlink.exe --stream-url` for live-stream focused providers
- direct passthrough for already-playable media or network stream URLs

Auto mode chooses a preferred resolver based on the provider and URL, then tries
the other resolver if the first one fails. The resolved URL is passed to the
private OBS Media Source as its input.

High-quality download/remux mode uses yt-dlp's split-stream format selection:

```text
bestvideo[height<=1080][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=1080]+bestaudio/best[height<=1080]/best
```

The selected video and audio streams are merged by bundled FFmpeg into the
plugin cache folder, then the private OBS Media Source opens that local file.
This is the mode to use when YouTube or another provider only exposes 1080p+
quality as separate video-only and audio-only streams.

### Bundled Dependencies

Windows releases bundle pinned versions of yt-dlp and the official portable
Streamlink runtime. The Git repository does not commit generated third-party
executables. Instead, `scripts/fetch-windows-dependencies.ps1` downloads the
pinned artifacts and verifies SHA-256 hashes before placing them under
`data/bin`.

On Windows, every bundled resolver process launched by the plugin is assigned
to a cleanup job owned by OBS. Closing or terminating OBS closes that job and
kills the bundled resolver process tree, including FFmpeg children spawned by
yt-dlp.

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
stream in **Stream immediately** mode. Use **Download/remux high quality** when
you want yt-dlp and FFmpeg to merge those separate streams into one local file
for higher quality playback.

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

Use **Stream immediately** for live streams. Download/remux mode is intentionally
for finite videos and VODs because live streams do not produce a complete file
that can be handed to OBS.

## Limitations

The plugin does not bypass DRM, subscriptions, geographic restrictions, or
provider access controls. Browser cookies can be selected for content that
requires an authenticated session when using yt-dlp. See
`THIRD-PARTY-NOTICES.md` for bundled component versions and licenses.
