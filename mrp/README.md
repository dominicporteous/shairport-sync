# Native AirPlay Media Remote controls

Build the `development` branch with `--with-airplay-2 --with-mrp-interface` to
serve Apple's Media Remote Protocol over the paired AirPlay data stream (type
130). Install `libprotobuf-c-dev` in addition to the usual AirPlay 2 dependencies.
The checked-in protobuf C files avoid requiring a compiler for normal builds.
To regenerate them, install `protobuf-c-compiler` and run:

```
protoc-c --c_out=. mrp/messages.proto
```

The wire field definitions and data stream envelope are documented by
[pyatv](https://github.com/postlund/pyatv/tree/master/pyatv/protocols/mrp).
The endpoint uses the existing HomeKit pair verification and encrypted data
channel; it does not open an unauthenticated MRP service. The version string
includes `-mrp` when this support is compiled in.

Some Apple Home hubs establish only the encrypted AirPlay event channel and never
request a type-130 stream. For those remote-control sessions, the receiver now
relays source metadata, supported commands and playback state as `/command`
plists on their event channel. Controller `sendMediaRemoteCommand` requests on the
RTSP connection are authenticated and forwarded to the active audio source. The
event relay preserves the source's now-playing plist fields and uses the same
command formats observed in iOS source captures. Acceptance and display by a real
Apple Home hub remain device-validation requirements; a simulated acknowledgement
does not prove that Home consumes or displays these messages.

A controller can request device information, subscribe to now-playing updates,
and request the current playback queue item. Source `updateMRNowPlayingInfo`,
`updateMRSupportedCommands`, and `updateMRPlaybackState` messages are reflected
in MRP state updates. iOS sources that report play/pause only through
`kMRMediaRemoteNowPlayingInfoPlaybackRate` also update the playing/paused state.
Late subscribers receive the current state. Metadata
replacement clears the previous track; source disconnection clears its state.
Artwork is delivered when requested by the controller. Elapsed time timestamps
use Apple's reference epoch and remain attached to the source's elapsed value.

Play, pause, toggle, stop, next/previous track, scan, and seek commands are
translated into the existing AirPlay event-channel commands. Only commands
explicitly enabled by the current source are advertised or forwarded. Invalid,
unsupported, and disconnected commands return an error instead of claiming
success. Seek positions are seconds and must be finite and nonnegative.

The endpoint exposes the currently playing AirPlay source, rather than a local
music library or a fabricated playback queue. The source application must supply
rich metadata and supported commands. Enabling this endpoint cannot force Apple
Home to display controls that a particular OS version does not support.

Run the standalone protocol regression tests from the checkout:

```
./tests/mrp_test.sh
./tests/mrp_channel_test.sh build
```

The channel test links the configured AirPlay 2 + MRP build and exercises the
production receiver using real HAP encryption, including data-channel and
event-only metadata delivery and a simulated source acknowledging a seek command
through each control path. It does not simulate HomeKit pair verification.

`tests/fixtures/mrp-ios-supported-commands.xml` is a sanitised iOS capability
capture. It retains command numbers and enabled-flag types, with device identity
and optional source-specific parameters removed.

At log verbosity 2, look for `MRP source state observed`, `MRP event snapshot`,
`MRP event control`, and (when type 130 is used) `MRP data channel accepted` and
`MRP data message type`. These messages identify the transport and command result
without printing pairing secrets or metadata contents.

Device validation requires NQPTP, an AirPlay source, and a paired Apple controller.
Add the receiver in Apple Home, begin playback, then check title/artist/artwork,
play/pause, next/previous and seeking for a seekable track. Test a controller
joining during playback, track replacement, source disconnection, and unsupported
commands. This hardware validation is separate from the portable protocol tests.
