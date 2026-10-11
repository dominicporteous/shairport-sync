#include "mrp/state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static plist_t command(const char *type, plist_t params) {
  plist_t c = plist_new_dict();
  plist_dict_set_item(c, "type", plist_new_string(type));
  plist_dict_set_item(c, "params", params);
  return c;
}
static void observe_info(unsigned source, const char *policy, const char *key, plist_t value) {
  plist_t p = plist_new_dict(), info = plist_new_dict();
  plist_dict_set_item(p, "type", plist_new_string("npi-text"));
  plist_dict_set_item(p, "mergePolicy", plist_new_string(policy));
  plist_dict_set_item(info, key, value);
  plist_dict_set_item(p, "params", info);
  plist_t c = command("updateMRNowPlayingInfo", p);
  mrp_observe(source, c); plist_free(c);
}
static void enable_command(unsigned source, unsigned number, int enabled) {
  plist_t record = plist_new_dict();
  plist_dict_set_item(record, "kCommandInfoCommandKey", plist_new_uint(number));
  plist_dict_set_item(record, "kCommandInfoEnabledKey", plist_new_bool(enabled));
  char *data = NULL; uint32_t size = 0; plist_to_bin(record, &data, &size); plist_free(record);
  plist_t array = plist_new_array(); plist_array_append_item(array, plist_new_data(data, size));
  free(data);
  plist_t params = plist_new_dict(); plist_dict_set_item(params, "mrSupportedCommandsFromSender", array);
  plist_t c = command("updateMRSupportedCommands", params); mrp_observe(source, c); plist_free(c);
}
int main(void) {
  mrp_snapshot snapshot;
  mrp_get_snapshot(&snapshot);
  assert(!mrp_command_enabled(&snapshot, 1));
  mrp_free_snapshot(&snapshot);
  observe_info(7, "replace", "kMRMediaRemoteNowPlayingInfoTitle", plist_new_string("First track"));
  observe_info(7, "update", "kMRMediaRemoteNowPlayingInfoArtist", plist_new_string("Artist"));
  observe_info(7, "update", "kMRMediaRemoteNowPlayingInfoDuration", plist_new_real(180.0));
  observe_info(7, "update", "kMRMediaRemoteNowPlayingInfoElapsedTime", plist_new_real(45.0));
  observe_info(7, "update", "kMRMediaRemoteNowPlayingInfoTimestamp", plist_new_date(123, 500000));
  observe_info(7, "update", "kMRMediaRemoteNowPlayingInfoArtworkData", plist_new_data("image", 5));
  enable_command(7, 24, 1);
  mrp_get_snapshot(&snapshot);
  assert(mrp_command_enabled(&snapshot, 45));
  assert(!mrp_command_enabled(&snapshot, 1));
  assert(!mrp_command_enabled(&snapshot, 999));
  Mrp__Protocol *message = mrp_state_message(&snapshot, 1);
  assert(message && message->type == 4);
  assert(!strcmp(message->state->now_playing->title, "First track"));
  assert(!strcmp(message->state->now_playing->artist, "Artist"));
  assert(message->state->now_playing->duration == 180);
  assert(message->state->now_playing->elapsed == 45);
  assert(message->state->now_playing->timestamp == 123.5);
  assert(message->state->queue->items[0]->artwork.len == 5);
  assert(message->state->supported_commands->n_commands == 1);
  assert(message->state->supported_commands->commands[0]->command == 45);
  size_t length = 0, frame_length = 0;
  uint8_t *frame = mrp_frame(message, UINT64_C(0x1020304050607080), &length);
  assert(frame && !mrp_frame_size(frame, &frame_length) && length == frame_length);
  const char *fixture = getenv("MRP_TEST_FRAME");
  if (fixture) {
    FILE *file = fopen(fixture,"wb"); assert(file);
    assert(fwrite(frame,1,length,file) == length); assert(!fclose(file));
  }
  assert(!memcmp(frame + 4, "sync", 4) && !memcmp(frame + 16, "comm", 4));
  const uint8_t sequence[] = {0x10,0x20,0x30,0x40,0x50,0x60,0x70,0x80};
  assert(!memcmp(frame + 20, sequence, 8));
  plist_t root = NULL; plist_from_bin((char *)frame + 32, length - 32, &root);
  assert(root);
  char *bytes = NULL; uint64_t size = 0;
  plist_get_data_val(plist_dict_get_item(plist_dict_get_item(root,"params"),"data"), &bytes, &size);
  const uint8_t *cursor = (uint8_t *)bytes; size_t remaining = size; uint64_t count = 0;
  assert(!mrp_varint(&cursor, &remaining, &count) && count == remaining);
  Mrp__Protocol *decoded = mrp__protocol__unpack(NULL, count, cursor);
  assert(decoded && !strcmp(decoded->state->queue->items[0]->metadata->artist, "Artist"));
  mrp__protocol__free_unpacked(decoded, NULL);
  free(bytes); free(frame); plist_free(root);
  mrp__protocol__free_unpacked(message, NULL);
  message = mrp_state_message(&snapshot, 0);
  assert(!message->state->queue->items[0]->has_artwork);
  mrp__protocol__free_unpacked(message, NULL); mrp_free_snapshot(&snapshot);
  observe_info(7, "replace", "kMRMediaRemoteNowPlayingInfoTitle", plist_new_string("Second track"));
  mrp_get_snapshot(&snapshot); message = mrp_state_message(&snapshot, 1);
  assert(!message->state->now_playing->artist && !message->state->queue->items[0]->has_artwork);
  mrp__protocol__free_unpacked(message, NULL); mrp_free_snapshot(&snapshot);
  enable_command(7, 0, 0);
  mrp_get_snapshot(&snapshot); assert(!mrp_command_enabled(&snapshot, 1)); mrp_free_snapshot(&snapshot);
  observe_info(8, "update", "kMRMediaRemoteNowPlayingInfoTitle", plist_new_string("New source"));
  mrp_clear(7); mrp_get_snapshot(&snapshot); assert(snapshot.source == 8); mrp_free_snapshot(&snapshot);
  // Sanitised iOS capture: zero-based commands, boolean enabled flags and data plists.
  FILE *fixture_file = fopen("tests/fixtures/mrp-ios-supported-commands.xml","rb"); assert(fixture_file);
  assert(!fseek(fixture_file,0,SEEK_END)); long fixture_size = ftell(fixture_file); assert(fixture_size > 0);
  rewind(fixture_file); char *fixture_xml = malloc(fixture_size); assert(fixture_xml);
  assert(fread(fixture_xml,1,fixture_size,fixture_file) == (size_t)fixture_size); fclose(fixture_file);
  plist_t captured = NULL; plist_from_xml(fixture_xml,fixture_size,&captured); free(fixture_xml); assert(captured);
  mrp_observe(8,captured);plist_free(captured);
  observe_info(8,"update","kMRMediaRemoteNowPlayingInfoPlaybackRate",plist_new_real(1.0));
  mrp_get_snapshot(&snapshot); assert(snapshot.playback_state == 1);
  assert(mrp_command_enabled(&snapshot,1) && mrp_command_enabled(&snapshot,45));
  plist_t event_info = mrp_event_info(&snapshot), event_caps = mrp_event_capabilities(&snapshot);
  plist_t event_state = mrp_event_playback(&snapshot);
  assert(plist_get_node_type(plist_dict_get_item(plist_dict_get_item(event_info,"params"),"params")) == PLIST_DICT);
  assert(plist_array_get_size(plist_dict_get_item(plist_dict_get_item(event_caps,"params"),
                                               "mrSupportedCommandsFromSender")) == 13);
  uint64_t state_number = 0;
  plist_get_uint_val(plist_dict_get_item(plist_dict_get_item(event_state,"params"),"mrPlaybackState"),&state_number);
  assert(state_number == 1);
  plist_free(event_info);plist_free(event_caps);plist_free(event_state);mrp_free_snapshot(&snapshot);
  observe_info(8,"update","kMRMediaRemoteNowPlayingInfoPlaybackRate",plist_new_real(0.0));
  mrp_get_snapshot(&snapshot); assert(snapshot.playback_state == 2);mrp_free_snapshot(&snapshot);
  observe_info(8,"update","kMRMediaRemoteNowPlayingInfoArtist",plist_new_string("Paused artist"));
  mrp_get_snapshot(&snapshot); assert(snapshot.playback_state == 2);mrp_free_snapshot(&snapshot);
  observe_info(8,"replace","kMRMediaRemoteNowPlayingInfoTitle",plist_new_string("New track"));
  mrp_get_snapshot(&snapshot); assert(snapshot.playback_state == 0);mrp_free_snapshot(&snapshot);
  mrp_clear(8); mrp_get_snapshot(&snapshot);
  message = mrp_state_message(&snapshot, 1);
  assert(!message->state->queue->n_items && !message->state->supported_commands->n_commands);
  mrp__protocol__free_unpacked(message, NULL); mrp_free_snapshot(&snapshot);
  uint8_t header[32] = {0};
  assert(mrp_frame_size(header, &length) == -1);
  header[0] = 0x7f; assert(mrp_frame_size(header, &length) == -1);
  uint8_t invalid[] = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
  cursor = invalid; remaining = sizeof(invalid); assert(mrp_varint(&cursor, &remaining, &count) == -1);
  cursor = invalid; remaining = 1; assert(mrp_varint(&cursor, &remaining, &count) == -1);
  for (unsigned i = 1; i <= 12; i++) assert(mrp_command_number(i) == (int)i-1);
  assert(mrp_command_number(45) == 24 && mrp_command_number(0) == -1);
  puts("MRP protocol regression tests passed: metadata, artwork, subscriptions snapshots, controls, lifecycle, framing and malformed input.");
  return 0;
}
