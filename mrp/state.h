#pragma once
#include <stddef.h>
#include <stdint.h>
#include <plist/plist.h>
#include "messages.pb-c.h"

typedef struct {
  unsigned source;
  uint64_t revision;
  plist_t info;
  plist_t commands;
  uint32_t playback_state;
} mrp_snapshot;

// All stored plists are copied. The caller must be the current audio source.
void mrp_observe(unsigned source, plist_t command);
void mrp_clear(unsigned source);
void mrp_get_snapshot(mrp_snapshot *snapshot);
void mrp_free_snapshot(mrp_snapshot *snapshot);
int mrp_command_number(uint32_t command);
int mrp_command_enabled(const mrp_snapshot *snapshot, uint32_t command);
// AirPlay /command plists for an event-only controller; caller owns each plist.
plist_t mrp_event_info(const mrp_snapshot *snapshot);
plist_t mrp_event_capabilities(const mrp_snapshot *snapshot);
plist_t mrp_event_playback(const mrp_snapshot *snapshot);
// A standalone, owned protobuf message, freed with mrp__protocol__free_unpacked.
Mrp__Protocol *mrp_state_message(const mrp_snapshot *snapshot, int artwork);
Mrp__Protocol *mrp_player_message(uint32_t type);
// DataStream envelopes: 32-byte big-endian header, binary plist, varint messages.
uint8_t *mrp_frame(const Mrp__Protocol *message, uint64_t sequence, size_t *length);
int mrp_frame_size(const uint8_t header[32], size_t *length);
int mrp_varint(const uint8_t **data, size_t *length, uint64_t *value);
