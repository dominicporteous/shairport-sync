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
channel; it does not open an unauthenticated MRP service.

A controller can request device information, subscribe to now-playing updates,
and request the current playback queue item. Source `updateMRNowPlayingInfo`,
`updateMRSupportedCommands`, and `updateMRPlaybackState` messages are reflected
in MRP state updates. Late subscribers receive the current state. Metadata
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
production receiver using real HAP encryption, including a simulated source
acknowledging a seek command. It does not simulate HomeKit pair verification.

Device validation requires NQPTP, an AirPlay source, and a paired Apple controller.
Add the receiver in Apple Home, begin playback, then check title/artist/artwork,
play/pause, next/previous and seeking for a seekable track. Test a controller
joining during playback, track replacement, source disconnection, and unsupported
commands. This hardware validation is separate from the portable protocol tests.
