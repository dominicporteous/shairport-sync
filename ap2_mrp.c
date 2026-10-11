/* AirPlay 2 encrypted Media Remote data channel. See mrp/README.md. */
#include "config.h"
#include "ap2_mrp.h"
#include "ap2_event_receiver.h"
#include "common.h"
#include "utilities/network_utilities.h"
#include "rtsp.h"
#include "mrp/state.h"
#include "utilities/generate_random_uuid.h"
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

// A single thread owns this channel's socket, encryption counters and subscription.
typedef struct {
  rtsp_conn_info *conn;
  int fd;
  int subscribed;
  int artwork;
  uint64_t revision;
  uint64_t sequence;
} mrp_channel;

static int send_message(mrp_channel *channel, Mrp__Protocol *message) {
  char *uuid = generate_random_uuid();
  message->unique_identifier = uuid;
  size_t length = 0;
  uint8_t *frame = mrp_frame(message, channel->sequence++, &length);
  free(uuid); message->unique_identifier = NULL;
  if (!frame) return -1;
  ssize_t written = write_encrypted(channel->fd,
      &channel->conn->ap2_pairing_context.data_cipher_bundle, frame, length);
  free(frame);
  return written == (ssize_t)length ? 0 : -1;
}
static int send_owned(mrp_channel *channel, Mrp__Protocol *message) {
  if (!message) return -1;
  int result = send_message(channel, message);
  mrp__protocol__free_unpacked(message, NULL);
  return result;
}
static int send_state(mrp_channel *channel, int force) {
  mrp_snapshot snapshot;
  mrp_get_snapshot(&snapshot);
  int result = 0;
  if (force || snapshot.revision != channel->revision) {
    result = send_owned(channel, mrp_state_message(&snapshot, channel->artwork));
    if (!result) channel->revision = snapshot.revision;
  }
  mrp_free_snapshot(&snapshot);
  return result;
}

void ap2_mrp_observe(rtsp_conn_info *conn, plist_t command) {
  pthread_rwlock_rdlock(&principal_conn_lock);
  if (conn == principal_conn && conn->airplay_type == ap_2) {
    mrp_observe(conn->connection_number, command);
    debug(2, "Connection %d: MRP source state observed.", conn->connection_number);
  }
  pthread_rwlock_unlock(&principal_conn_lock);
}

static int forward_command(Mrp__SendCommand *request) {
  if (!request || !request->has_command) return 7; // InvalidOptions
  if (request->path &&
      ((request->path->client && request->path->client->bundle_identifier &&
        strcmp(request->path->client->bundle_identifier, "org.gnome.ShairportSync")) ||
       (request->path->player && request->path->player->identifier &&
        strcmp(request->path->player->identifier, "AirPlay")))) return 7;
  int command = mrp_command_number(request->command);
  if (command < 0) return 10; // NotSupported
  if (request->command == 45 && (!request->options || !request->options->has_position ||
      !isfinite(request->options->position) || request->options->position < 0)) return 7;
  mrp_snapshot snapshot;
  mrp_get_snapshot(&snapshot);
  int enabled = mrp_command_enabled(&snapshot, request->command);
  unsigned source = snapshot.source;
  mrp_free_snapshot(&snapshot);
  if (!enabled) return 10;

  int error = 2; // ConnectionFailed
  pthread_rwlock_rdlock(&principal_conn_lock);
  pthread_cleanup_push(rwlock_unlock, &principal_conn_lock);
  rtsp_conn_info *conn = principal_conn;
  if (conn && (unsigned)conn->connection_number == source && conn->airplay_type == ap_2 &&
      conn->airplay_gid && conn->event_channel_fd > 0) {
    plist_t command_plist = plist_new_dict(), params = plist_new_dict();
    char number[24]; snprintf(number, sizeof(number), "%d", command);
    plist_dict_set_item(command_plist, "type", plist_new_string("sendMediaRemoteCommand"));
    plist_dict_set_item(command_plist, "modernMediaRemoteCommand", plist_new_string(number));
    char *uuid = generate_random_uuid();
    plist_dict_set_item(params, "kMRMediaRemoteOptionCommandID", plist_new_string(uuid));
    free(uuid);
    plist_dict_set_item(params, "kMRMediaRemoteOptionSendOptionsNumber", plist_new_uint(0));
    plist_dict_set_item(params, "kMRMediaRemoteOptionIsRedirectingCommand", plist_new_bool(1));
    // Use the same destination archive as the existing local remote-control path.
    extern plist_t destinationArchive(const char *deviceUUID);
    plist_dict_set_item(params, "kMRMediaRemoteOptionDestinationDeviceUIDs",
                         destinationArchive(conn->airplay_gid));
    if (request->command == 45)
      plist_dict_set_item(params, "kMRMediaRemoteOptionPlaybackPosition",
                           plist_new_real(request->options->position));
    plist_dict_set_item(command_plist, "params", params);
    error = ap2_event_port_post_command(conn, command_plist) > 0 ? 0 : 2;
    plist_free(command_plist);
  }
  pthread_cleanup_pop(1);
  return error;
}

int ap2_mrp_send_event_state(rtsp_conn_info *conn, uint64_t *revision, int force) {
  if (conn->airplay_stream_category != remote_control_stream ||
      !conn->ap2_pairing_context.control_cipher_bundle.is_encrypted) return 0;
  mrp_snapshot snapshot; mrp_get_snapshot(&snapshot);
  int result = 0;
  if (force || snapshot.revision != *revision) {
    plist_t messages[] = {mrp_event_capabilities(&snapshot), mrp_event_info(&snapshot),
                          mrp_event_playback(&snapshot)};
    for (unsigned i = 0; i < 3; i++) {
      if (!result && ap2_event_port_post_command(conn, messages[i]) <= 0) result = -1;
      plist_free(messages[i]);
    }
    if (!result) *revision = snapshot.revision;
    debug(2, "Connection %d: MRP event snapshot revision %" PRIu64 " sent, result %d.",
          conn->connection_number, snapshot.revision, result);
  }
  mrp_free_snapshot(&snapshot);
  return result;
}

int ap2_mrp_handle_event_command(rtsp_conn_info *conn, plist_t command, rtsp_message *response) {
  if (conn->airplay_stream_category != remote_control_stream) return 0;
  if (plist_get_node_type(command) != PLIST_DICT) { response->respcode = 451; return 1; }
  char *type = NULL;
  plist_t type_node = plist_dict_get_item(command, "type");
  if (plist_get_node_type(type_node) == PLIST_STRING) plist_get_string_val(type_node, &type);
  int control = type && strcmp(type, "sendMediaRemoteCommand") == 0;
  free(type);
  if (!control) return 0;
  if (!conn->ap2_pairing_context.control_cipher_bundle.is_encrypted || !conn->pair_setup_result ||
      !conn->pair_setup_result->shared_secret_len) { response->respcode = 470; return 1; }
  char *number = NULL;
  plist_t number_node = plist_dict_get_item(command, "modernMediaRemoteCommand");
  if (plist_get_node_type(number_node) == PLIST_STRING) plist_get_string_val(number_node, &number);
  if (!number || !*number || strspn(number, "0123456789") != strlen(number) || strlen(number) > 2) {
    free(number); response->respcode = 451; return 1;
  }
  unsigned modern = strtoul(number, NULL, 10); free(number);
  if (modern > 11 && modern != 24) { response->respcode = 501; return 1; }
  Mrp__SendCommand request = MRP__SEND_COMMAND__INIT;
  request.has_command = 1; request.command = modern == 24 ? 45 : modern + 1;
  Mrp__Options options = MRP__OPTIONS__INIT;
  if (modern == 24) {
    plist_t params = plist_dict_get_item(command, "params");
    plist_t position = plist_get_node_type(params) == PLIST_DICT ?
                       plist_dict_get_item(params, "kMRMediaRemoteOptionPlaybackPosition") : NULL;
    if (plist_get_node_type(position) == PLIST_REAL) {
      options.has_position = 1; plist_get_real_val(position, &options.position);
    } else if (plist_get_node_type(position) == PLIST_UINT) {
      uint64_t n = 0; plist_get_uint_val(position, &n); options.has_position = 1; options.position = n;
    }
    request.options = &options;
  }
  int error = forward_command(&request);
  response->respcode = !error ? 200 : error == 7 ? 451 : error == 10 ? 501 : 503;
  debug(2, "Connection %d: MRP event control %u returned RTSP %d.",
        conn->connection_number, modern, response->respcode);
  return 1;
}

static int handle_message(mrp_channel *channel, Mrp__Protocol *request) {
  if (!request->has_type) return -1;
  debug(2, "Connection %d: MRP data message type %u.", channel->conn->connection_number, request->type);
  Mrp__Protocol reply = MRP__PROTOCOL__INIT;
  reply.has_type = 1; reply.type = request->type; reply.identifier = request->identifier;
  Mrp__DeviceInfo device = MRP__DEVICE_INFO__INIT;
  Mrp__CommandResult result = MRP__COMMAND_RESULT__INIT;
  switch (request->type) {
  case 15: // DEVICE_INFO
    {
    device.name = config.service_name;
    device.unique_identifier = config.airplay_pi;
    device.device_uid = config.airplay_pi;
    device.model = "Shairport Sync";
    device.bundle_identifier = "org.gnome.ShairportSync";
    device.has_protocol_version = 1; device.protocol_version = 1;
    device.has_last_supported_message_type = 1; device.last_supported_message_type = 120;
    device.has_connected = 1; device.connected = 1;
    device.has_device_class = 1; device.device_class = 7; // Accessory
    mrp_snapshot snapshot; mrp_get_snapshot(&snapshot);
    device.has_is_airplay_active = 1; device.is_airplay_active = snapshot.source != 0;
    mrp_free_snapshot(&snapshot);
    reply.device_info = &device;
    return send_message(channel, &reply);
    }
  case 16: // CLIENT_UPDATES_CONFIG
    channel->subscribed = request->updates && request->updates->has_now_playing &&
                          request->updates->now_playing;
    channel->artwork = request->updates && request->updates->has_artwork && request->updates->artwork;
    if (send_message(channel, &reply)) return -1;
    if (channel->subscribed) {
      if (send_owned(channel, mrp_player_message(46)) ||
          send_owned(channel, mrp_player_message(47))) return -1;
      return send_state(channel, 1);
    }
    return 0;
  case 3: // GET_STATE
  case 32: // PLAYBACK_QUEUE_REQUEST -- expose the current item; no invented queue
    {
      mrp_snapshot snapshot; mrp_get_snapshot(&snapshot);
      Mrp__Protocol *state = mrp_state_message(&snapshot, channel->artwork);
      mrp_free_snapshot(&snapshot);
      if (!state) return -1;
      state->identifier = request->identifier; // borrowed until serialisation
      int rc = send_message(channel, state);
      state->identifier = NULL;
      mrp__protocol__free_unpacked(state, NULL);
      return rc;
    }
  case 1: // SEND_COMMAND
    reply.type = 2;
    result.has_error = 1; result.error = forward_command(request->send_command);
    result.has_status = 1; result.status = result.error ? 2 : 0;
    reply.command_result = &result;
    return send_message(channel, &reply);
  case 24: // GET_KEYBOARD_SESSION -- this speaker has no keyboard
    reply.type = 23;
    return send_message(channel, &reply);
  case 38: // SET_CONNECTION_STATE
  case 120: // CONFIGURE_CONNECTION
    return request->identifier ? send_message(channel, &reply) : 0;
  default:
    reply.has_error_code = 1; reply.error_code = 6; // UnsupportedOperation
    return request->identifier ? send_message(channel, &reply) : 0;
  }
}

static int process_payload(mrp_channel *channel, const uint8_t *bytes, size_t length) {
  if (!length) return 0;
  plist_t root = NULL;
  if (length < 8 || memcmp(bytes, "bplist00", 8)) return -1;
  plist_from_bin((const char *)bytes, length, &root);
  if (plist_get_node_type(root) != PLIST_DICT) { if (root) plist_free(root); return -1; }
  plist_t params = plist_dict_get_item(root, "params");
  plist_t data = plist_get_node_type(params) == PLIST_DICT ? plist_dict_get_item(params, "data") : NULL;
  char *buffer = NULL; uint64_t size = 0;
  if (plist_get_node_type(data) == PLIST_DATA) plist_get_data_val(data, &buffer, &size);
  plist_free(root);
  if (!buffer || size > 4U * 1024U * 1024U) { free(buffer); return -1; }
  const uint8_t *cursor = (uint8_t *)buffer;
  size_t remaining = size;
  int rc = 0;
  while (remaining) {
    uint64_t n = remaining;
    // ConfigureConnection is sometimes sent without a length prefix.
    if (*cursor != 8 && mrp_varint(&cursor, &remaining, &n)) { rc = -1; break; }
    if (!n || n > remaining) { rc = -1; break; }
    Mrp__Protocol *message = mrp__protocol__unpack(NULL, n, cursor);
    if (!message) { rc = -1; break; }
    rc = handle_message(channel, message);
    mrp__protocol__free_unpacked(message, NULL);
    if (rc) break;
    cursor += n; remaining -= n;
  }
  free(buffer);
  return rc;
}
static int read_exact(mrp_channel *channel, uint8_t *buffer, size_t length) {
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  while (length) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec >= 2) return -1;
    ssize_t n = read_encrypted(channel->fd, &channel->conn->ap2_pairing_context.data_cipher_bundle,
                               buffer, length);
    if (n <= 0) return -1;
    buffer += n; length -= n;
  }
  return 0;
}
static void channel_cleanup(void *arg) {
  mrp_channel *channel = arg;
  if (channel->fd >= 0) close(channel->fd);
}
void *ap2_mrp_receiver(void *arg) {
  rtsp_conn_info *conn = arg;
  mrp_channel channel = {.conn = conn, .fd = -1, .sequence = UINT64_C(0x100000000)};
  pthread_cleanup_push(channel_cleanup, &channel);
  SOCKADDR address; socklen_t length = sizeof(address);
  channel.fd = eintr_checked_accept(conn->data_socket, (struct sockaddr *)&address, &length);
  if (channel.fd >= 0) {
    debug(2, "Connection %d: MRP data channel accepted.", conn->connection_number);
    struct timeval timeout = {.tv_sec = 2};
    setsockopt(channel.fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(channel.fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    while (1) {
      struct pollfd fd = {.fd = channel.fd, .events = POLLIN};
      int ready = poll(&fd, 1, 100);
      if (ready < 0) { if (errno == EINTR) continue; break; }
      if (ready && !(fd.revents & POLLIN)) break;
      // Bound allocations and defer cancellation until they have been released.
      int old_state; pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_state);
      int rc = 0;
      if (ready || conn->ap2_pairing_context.data_cipher_bundle.plaintext_read_buffer.length) {
        uint8_t header[32]; size_t size = 0;
        rc = read_exact(&channel, header, sizeof(header));
        if (!rc) rc = mrp_frame_size(header, &size);
        if (!rc && memcmp(header + 4, "sync", 4) == 0) {
          uint8_t *body = malloc(size - 32 ? size - 32 : 1);
          if (!body) rc = -1;
          else {
            rc = read_exact(&channel, body, size - 32);
            if (!rc) {
              uint8_t reply[32] = {0}; reply[3] = 32;
              memcpy(reply + 4, "rply", 4); memcpy(reply + 20, header + 20, 8);
              if (write_encrypted(channel.fd, &conn->ap2_pairing_context.data_cipher_bundle,
                                   reply, sizeof(reply)) != sizeof(reply)) rc = -1;
              if (!rc) rc = process_payload(&channel, body, size - 32);
            }
            free(body);
          }
        } else if (!rc && (memcmp(header + 4, "rply", 4) != 0 || size != 32)) rc = -1;
      }
      if (!rc && channel.subscribed) rc = send_state(&channel, 0);
      pthread_setcancelstate(old_state, NULL);
      if (rc) {
        debug(1, "Connection %d: MRP data channel closed after processing error %d (errno %d).",
              conn->connection_number, rc, errno);
        break;
      }
    }
  }
  pthread_cleanup_pop(1);
  return NULL;
}
