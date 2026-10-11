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

HomeKit setup and administration (`X-Apple-HKP: 6` or `7`) use the receiver UUID
returned by `/configure` when signing pairing responses. The existing advertised
public key is retained. Successful permanent pair verification now prepares the
encrypted control and event channels after the plaintext M4 response. Controller
signatures must match enrolled public keys; unknown controllers are rejected.
Ordinary AirPlay pairing continues to use the MAC identifier.

To retain Home enrollment across process or container restarts, add a unique file
to each receiver's `general` block, for example:

```
homekit_pairing_file = "/runtime/homekit/office.plist";
```

Create the parent directory and make it writable by Shairport Sync. Mount it on
persistent storage (a Kubernetes PVC, rather than an `emptyDir`, for example).
Use a different file for Upstairs and keep each receiver's `airplay_device_id`
stable. Writes atomically replace a file with mode 0600. The file stores enrolled
controller public keys and the Home access flag, bound to the receiver identity;
it contains no session secrets. Corrupt files or files belonging to another
receiver cause startup to fail rather than discard existing enrollment.
Without this setting, enrollment remains in memory and is lost at restart.
Previous builds cannot export their in-memory records: remove and re-add the
speaker in Home once after upgrading and enabling persistent storage.

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

Apple TV's own controls in Home are not a compatibility test for a third-party
AirPlay speaker. Apple TV also implements additional services, including
Companion for TV remote functions. Older tvOS versions expose standalone MRP via
`_mediaremotetv._tcp`; newer versions use MRP over AirPlay. This build implements
the AirPlay data-stream and event paths, without claiming Apple TV identity or
implementing Companion. The captured Home hub sessions so far request only an
event channel. An event acknowledgement proves transport acceptance, not Home
UI support. A working Home control session must be compared before attributing
missing rich controls to a specific unimplemented negotiation or service.

Run the standalone protocol regression tests from the checkout:

```
./tests/mrp_test.sh
./tests/mrp_channel_test.sh build
./tests/homekit_pairing_test.sh build
```

The channel test links the configured AirPlay 2 + MRP build and exercises the
production receiver using real HAP encryption, including data-channel and
event-only metadata delivery and a simulated source acknowledging a seek command
through each control path. The pairing test exercises real SRP enrollment,
M1–M4 signature verification and the first encrypted control request against the
production RTSP handlers. It checks enrolled, unknown and forged controllers,
retained receiver keys, Home UUID and legacy MAC identities, and enrollment,
removal and Home access state across fresh processes. It also rejects corrupt
state and state belonging to a different receiver.

`tests/fixtures/mrp-ios-supported-commands.xml` is a sanitised iOS capability
capture. It retains command numbers and enabled-flag types, with device identity
and optional source-specific parameters removed.

At log verbosity 2, look for `MRP source state observed`, `MRP event snapshot`,
`MRP event control`, and (when type 130 is used) `MRP data channel accepted` and
`MRP data message type`. These messages identify the transport and command result
without printing pairing secrets or metadata contents.
For Home availability, first look for `HomeKit pair verification completed;
encryption prepared`. An acknowledged event snapshot at revision 0 represents
empty playback state; play to that specific receiver and confirm source-state
updates and later snapshot revisions before evaluating its controls.

Device validation requires NQPTP, an AirPlay source, and a paired Apple controller.
Add the receiver in Apple Home, begin playback, then check title/artist/artwork,
play/pause, next/previous and seeking for a seekable track. Test a controller
joining during playback, track replacement, source disconnection, and unsupported
commands. This hardware validation is separate from the portable protocol tests.
