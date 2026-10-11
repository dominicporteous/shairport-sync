#include "state.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_FRAME (4U * 1024U * 1024U)
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static mrp_snapshot current;

static int string_is(plist_t node, const char *value) {
  char *text = NULL;
  if (plist_get_node_type(node) == PLIST_STRING) plist_get_string_val(node, &text);
  int result = text && strcmp(text, value) == 0;
  free(text);
  return result;
}

static void merge(plist_t base, plist_t changes) {
  plist_dict_iter iter = NULL;
  plist_dict_new_iter(changes, &iter);
  char *key = NULL;
  plist_t value = NULL;
  for (;;) {
    plist_dict_next_item(changes, iter, &key, &value);
    if (!key) break;
    plist_dict_set_item(base, key, plist_copy(value));
    free(key);
  }
  free(iter);
}

void mrp_observe(unsigned source, plist_t command) {
  if (!source || plist_get_node_type(command) != PLIST_DICT) return;
  plist_t type = plist_dict_get_item(command, "type");
  plist_t params = plist_dict_get_item(command, "params");
  if (plist_get_node_type(params) != PLIST_DICT) return;
  int info = string_is(type, "updateMRNowPlayingInfo");
  int commands = string_is(type, "updateMRSupportedCommands");
  int state = string_is(type, "updateMRPlaybackState");
  plist_t value = NULL;
  if (info) {
    if (!string_is(plist_dict_get_item(params, "type"), "npi-text")) return;
    value = plist_dict_get_item(params, "params");
    if (plist_get_node_type(value) != PLIST_DICT) return;
  } else if (commands) {
    value = plist_dict_get_item(params, "mrSupportedCommandsFromSender");
    if (plist_get_node_type(value) != PLIST_ARRAY || plist_array_get_size(value) > 256) return;
  } else if (state) {
    value = plist_dict_get_item(params, "mrPlaybackState");
    if (plist_get_node_type(value) != PLIST_UINT) return;
  } else return;
  pthread_mutex_lock(&lock);
  if (current.source != source) {
    if (current.info) plist_free(current.info);
    if (current.commands) plist_free(current.commands);
    current.info = NULL;
    current.commands = NULL;
    current.playback_state = 0;
    current.source = source;
  }
  if (info) {
    if (!current.info || string_is(plist_dict_get_item(params, "mergePolicy"), "replace")) {
      if (current.info) plist_free(current.info);
      current.info = plist_copy(value);
      current.playback_state = plist_dict_get_size(value) ? 0 : 3;
    } else merge(current.info, value);
    // iOS sends rate changes in npi-text without updateMRPlaybackState.
    plist_t rate_node = plist_dict_get_item(value, "kMRMediaRemoteNowPlayingInfoPlaybackRate");
    if (plist_get_node_type(rate_node) == PLIST_REAL) {
      double rate = 0;
      plist_get_real_val(rate_node, &rate);
      if (isfinite(rate) && rate >= 0) current.playback_state = rate > 0 ? 1 : 2;
    }
  } else if (commands) {
    if (current.commands) plist_free(current.commands);
    current.commands = plist_copy(value);
  } else {
    uint64_t playback = 0;
    plist_get_uint_val(value, &playback);
    current.playback_state = playback <= 5 ? playback : 0;
  }
  current.revision++;
  pthread_mutex_unlock(&lock);
}

void mrp_clear(unsigned source) {
  pthread_mutex_lock(&lock);
  if (current.source == source) {
    if (current.info) plist_free(current.info);
    if (current.commands) plist_free(current.commands);
    current.info = NULL; current.commands = NULL; current.source = 0;
    current.playback_state = 3; current.revision++;
  }
  pthread_mutex_unlock(&lock);
}
void mrp_get_snapshot(mrp_snapshot *s) {
  pthread_mutex_lock(&lock);
  *s = current;
  s->info = current.info ? plist_copy(current.info) : NULL;
  s->commands = current.commands ? plist_copy(current.commands) : NULL;
  pthread_mutex_unlock(&lock);
}
void mrp_free_snapshot(mrp_snapshot *s) {
  if (s->info) plist_free(s->info);
  if (s->commands) plist_free(s->commands);
  memset(s, 0, sizeof(*s));
}

// MRP's enum and the AirPlay modernMediaRemoteCommand numbers differ.
int mrp_command_number(uint32_t command) {
  if (command >= 1 && command <= 12) return command - 1;
  if (command == 45) return 24; // SeekToPlaybackPosition
  return -1;
}
int mrp_command_enabled(const mrp_snapshot *s, uint32_t command) {
  int modern = mrp_command_number(command);
  if (!s->source || modern < 0 || !s->commands) return 0;
  for (uint32_t i = 0; i < plist_array_get_size(s->commands); i++) {
    plist_t item = plist_array_get_item(s->commands, i);
    plist_t decoded = NULL;
    if (plist_get_node_type(item) == PLIST_DATA) {
      char *bytes = NULL; uint64_t length = 0;
      plist_get_data_val(item, &bytes, &length);
      if (bytes && length <= MAX_FRAME) plist_from_bin(bytes, (uint32_t)length, &decoded);
      free(bytes);
      item = decoded;
    }
    int enabled = 0;
    if (plist_get_node_type(item) == PLIST_DICT) {
      plist_t number = plist_dict_get_item(item, "kCommandInfoCommandKey");
      plist_t flag = plist_dict_get_item(item, "kCommandInfoEnabledKey");
      uint64_t n = UINT64_MAX; uint8_t b = 0;
      if (plist_get_node_type(number) == PLIST_UINT) plist_get_uint_val(number, &n);
      if (plist_get_node_type(flag) == PLIST_BOOLEAN) plist_get_bool_val(flag, &b);
      enabled = n == (uint64_t)modern && b;
    }
    if (decoded) plist_free(decoded);
    if (enabled) return 1;
  }
  return 0;
}

static plist_t event_command(const char *type, plist_t params) {
  plist_t command = plist_new_dict();
  plist_dict_set_item(command, "type", plist_new_string(type));
  plist_dict_set_item(command, "params", params);
  return command;
}
plist_t mrp_event_info(const mrp_snapshot *s) {
  plist_t params = plist_new_dict();
  plist_dict_set_item(params, "type", plist_new_string("npi-text"));
  plist_dict_set_item(params, "mergePolicy", plist_new_string("replace"));
  plist_dict_set_item(params, "params", s->info ? plist_copy(s->info) : plist_new_dict());
  return event_command("updateMRNowPlayingInfo", params);
}
plist_t mrp_event_capabilities(const mrp_snapshot *s) {
  plist_t params = plist_new_dict(), array = plist_new_array();
  // Advertise only controls that this bridge can actually forward.
  for (unsigned i = 0; i < 13; i++) {
    unsigned command = i < 12 ? i + 1 : 45;
    if (!mrp_command_enabled(s, command)) continue;
    plist_t info = plist_new_dict();
    plist_dict_set_item(info, "kCommandInfoCommandKey", plist_new_uint(mrp_command_number(command)));
    plist_dict_set_item(info, "kCommandInfoEnabledKey", plist_new_bool(1));
    char *bytes = NULL; uint32_t length = 0;
    plist_to_bin(info, &bytes, &length); plist_free(info);
    if (bytes) plist_array_append_item(array, plist_new_data(bytes, length));
    free(bytes);
  }
  plist_dict_set_item(params, "mrSupportedCommandsFromSender", array);
  return event_command("updateMRSupportedCommands", params);
}
plist_t mrp_event_playback(const mrp_snapshot *s) {
  plist_t params = plist_new_dict();
  plist_dict_set_item(params, "mrPlaybackState", plist_new_uint(s->playback_state));
  return event_command("updateMRPlaybackState", params);
}

static char *text(plist_t info, const char *key) {
  char *s = NULL;
  plist_t node = info ? plist_dict_get_item(info, key) : NULL;
  if (plist_get_node_type(node) == PLIST_STRING) plist_get_string_val(node, &s);
  return s;
}
static int real(plist_t info, const char *key, double *value) {
  plist_t node = info ? plist_dict_get_item(info, key) : NULL;
  if (plist_get_node_type(node) == PLIST_REAL) plist_get_real_val(node, value);
  else if (plist_get_node_type(node) == PLIST_UINT) {
    uint64_t n = 0; plist_get_uint_val(node, &n); *value = n;
  } else return 0;
  return isfinite(*value) && *value >= 0;
}
static Mrp__Protocol *copy_message(Mrp__Protocol *message) {
  size_t n = mrp__protocol__get_packed_size(message);
  uint8_t *bytes = malloc(n);
  if (!bytes) return NULL;
  mrp__protocol__pack(message, bytes);
  Mrp__Protocol *result = mrp__protocol__unpack(NULL, n, bytes);
  free(bytes);
  return result;
}
static void player_path(Mrp__PlayerPath *path, Mrp__Client *client, Mrp__Player *player) {
  client->bundle_identifier = "org.gnome.ShairportSync";
  client->display_name = "Shairport Sync";
  player->identifier = "AirPlay";
  player->display_name = "AirPlay";
  player->has_is_default = 1; player->is_default = 1;
  path->client = client; path->player = player;
}
Mrp__Protocol *mrp_player_message(uint32_t type) {
  Mrp__Protocol message = MRP__PROTOCOL__INIT;
  Mrp__PlayerPath path = MRP__PLAYER_PATH__INIT;
  Mrp__Client client = MRP__CLIENT__INIT;
  Mrp__Player player = MRP__PLAYER__INIT;
  Mrp__ClientMessage cm = MRP__CLIENT_MESSAGE__INIT;
  Mrp__PlayerMessage pm = MRP__PLAYER_MESSAGE__INIT;
  player_path(&path, &client, &player);
  message.has_type = 1; message.type = type;
  if (type == 46) { cm.client = &client; message.client_message = &cm; }
  else { pm.path = &path; message.player_message = &pm; }
  return copy_message(&message);
}
Mrp__Protocol *mrp_state_message(const mrp_snapshot *s, int artwork) {
  Mrp__Protocol message = MRP__PROTOCOL__INIT;
  Mrp__State state = MRP__STATE__INIT;
  Mrp__NowPlaying npi = MRP__NOW_PLAYING__INIT;
  Mrp__ItemMetadata metadata = MRP__ITEM_METADATA__INIT;
  Mrp__Item item = MRP__ITEM__INIT;
  Mrp__Item *items[] = {&item};
  Mrp__Queue queue = MRP__QUEUE__INIT;
  Mrp__PlayerPath path = MRP__PLAYER_PATH__INIT;
  Mrp__Client client = MRP__CLIENT__INIT;
  Mrp__Player player = MRP__PLAYER__INIT;
  player_path(&path, &client, &player);
  message.has_type = 1; message.type = 4; message.state = &state;
  state.path = &path; state.now_playing = &npi;
  state.has_playback_state = 1; state.playback_state = s->playback_state;
  npi.title = text(s->info, "kMRMediaRemoteNowPlayingInfoTitle");
  npi.artist = text(s->info, "kMRMediaRemoteNowPlayingInfoArtist");
  npi.album = text(s->info, "kMRMediaRemoteNowPlayingInfoAlbum");
  npi.has_duration = real(s->info, "kMRMediaRemoteNowPlayingInfoDuration", &npi.duration);
  npi.has_elapsed = real(s->info, "kMRMediaRemoteNowPlayingInfoElapsedTime", &npi.elapsed);
  double rate = 0;
  npi.has_rate = real(s->info, "kMRMediaRemoteNowPlayingInfoPlaybackRate", &rate);
  npi.rate = rate;
  plist_t date = s->info ? plist_dict_get_item(s->info, "kMRMediaRemoteNowPlayingInfoTimestamp") : NULL;
  if (plist_get_node_type(date) == PLIST_DATE) {
    int32_t sec = 0, usec = 0;
    plist_get_date_val(date, &sec, &usec);
    npi.has_timestamp = 1; npi.timestamp = sec + usec / 1e6;
  }
  metadata.title = npi.title; metadata.artist = npi.artist; metadata.album = npi.album;
  metadata.has_duration = npi.has_duration; metadata.duration = npi.duration;
  metadata.has_elapsed = npi.has_elapsed; metadata.elapsed = npi.elapsed;
  metadata.has_rate = npi.has_rate; metadata.rate = npi.rate;
  metadata.has_elapsed_timestamp = npi.has_timestamp; metadata.elapsed_timestamp = npi.timestamp;
  metadata.has_media_type = 1; metadata.media_type = 1;
  char *art = NULL; uint64_t art_length = 0;
  plist_t art_node = s->info ? plist_dict_get_item(s->info, "kMRMediaRemoteNowPlayingInfoArtworkData") : NULL;
  if (artwork && plist_get_node_type(art_node) == PLIST_DATA) {
    plist_get_data_val(art_node, &art, &art_length);
    if (art && art_length < MAX_FRAME / 2) {
      item.has_artwork = 1; item.artwork.data = (uint8_t *)art; item.artwork.len = art_length;
      metadata.has_artwork_available = 1; metadata.artwork_available = 1;
    }
  }
  if (s->info) {
    item.identifier = "current"; item.metadata = &metadata;
    queue.has_location = 1; queue.location = 0; queue.n_items = 1; queue.items = items;
  }
  state.queue = &queue;
  Mrp__SupportedCommands supported = MRP__SUPPORTED_COMMANDS__INIT;
  Mrp__CommandInfo ci[13]; Mrp__CommandInfo *pointers[13];
  for (unsigned i = 0; i < 13; i++) {
    uint32_t number = i < 12 ? i + 1 : 45;
    if (mrp_command_enabled(s, number)) {
      ci[supported.n_commands] = (Mrp__CommandInfo)MRP__COMMAND_INFO__INIT;
      Mrp__CommandInfo *c = &ci[supported.n_commands];
      c->has_command = 1; c->command = number; c->has_enabled = 1; c->enabled = 1;
      pointers[supported.n_commands++] = c;
    }
  }
  supported.commands = pointers; state.supported_commands = &supported;
  Mrp__Protocol *result = copy_message(&message);
  free(npi.title); free(npi.artist); free(npi.album); free(art);
  return result;
}

int mrp_varint(const uint8_t **data, size_t *length, uint64_t *value) {
  *value = 0;
  for (unsigned shift = 0; shift < 64 && *length; shift += 7) {
    uint8_t b = *(*data)++; (*length)--;
    if (shift == 63 && (b & 0xfe)) return -1;
    *value |= (uint64_t)(b & 0x7f) << shift;
    if (!(b & 0x80)) return 0;
  }
  return -1;
}
int mrp_frame_size(const uint8_t h[32], size_t *length) {
  *length = ((size_t)h[0] << 24) | ((size_t)h[1] << 16) | ((size_t)h[2] << 8) | h[3];
  return *length >= 32 && *length <= MAX_FRAME ? 0 : -1;
}
uint8_t *mrp_frame(const Mrp__Protocol *message, uint64_t sequence, size_t *length) {
  size_t n = mrp__protocol__get_packed_size(message);
  if (n > MAX_FRAME / 2) return NULL;
  uint8_t *bytes = malloc(n + 10);
  if (!bytes) return NULL;
  size_t prefix = 0, remaining = n;
  do { bytes[prefix++] = (remaining & 127) | (remaining > 127 ? 128 : 0); remaining >>= 7; } while (remaining);
  mrp__protocol__pack(message, bytes + prefix);
  plist_t root = plist_new_dict(), params = plist_new_dict();
  plist_dict_set_item(params, "data", plist_new_data((char *)bytes, n + prefix));
  plist_dict_set_item(root, "params", params);
  free(bytes);
  char *body = NULL; uint32_t size = 0;
  plist_to_bin(root, &body, &size); plist_free(root);
  if (!body || size > MAX_FRAME - 32) { free(body); return NULL; }
  *length = size + 32;
  uint8_t *frame = calloc(1, *length);
  if (frame) {
    for (int i = 0; i < 4; i++) frame[i] = *length >> (24 - 8 * i);
    memcpy(frame + 4, "sync", 4); memcpy(frame + 16, "comm", 4);
    for (int i = 0; i < 8; i++) frame[20 + i] = sequence >> (56 - 8 * i);
    memcpy(frame + 32, body, size);
  }
  free(body);
  return frame;
}
