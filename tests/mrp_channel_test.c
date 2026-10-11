// Exercise the production data-channel thread with real HAP encryption.
#include "config.h"
#include "common.h"
#include "utilities/network_utilities.h"
#include "ap2_mrp.h"
#include "rtsp.h"
#include "utilities/rtsp_message_utilities.h"
extern void handle_setup_2(rtsp_conn_info *, rtsp_message *, rtsp_message *);
extern void handle_command(rtsp_conn_info *, rtsp_message *, rtsp_message *);
#include "mrp/state.h"
#include <assert.h>
#include <gcrypt.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static uint8_t secret[32] = {1,2,3,4,5};
static pair_cipher_bundle client_cipher;
static int client_fd;
static void exact(int fd, pair_cipher_bundle *cipher, uint8_t *data, size_t size) {
  while (size) {
    ssize_t n = read_encrypted(fd, cipher, data, size);
    assert(n > 0); data += n; size -= n;
  }
}
static void send_request(Mrp__Protocol *message) {
  size_t size = 0; uint8_t *data = mrp_frame(message, 12345, &size);
  assert(data);
  // Deliberately fragment an envelope across encrypted records.
  assert(write_encrypted(client_fd, &client_cipher, data, 11) == 11);
  assert(write_encrypted(client_fd, &client_cipher, data+11, size-11) == (ssize_t)size-11);
  free(data);
}
static Mrp__Protocol *receive_message(void) {
  uint8_t header[32]; size_t size;
  for (;;) {
    exact(client_fd, &client_cipher, header, 32);
    assert(!mrp_frame_size(header, &size));
    if (!memcmp(header+4,"rply",4)) { assert(size == 32); continue; }
    assert(!memcmp(header+4,"sync",4));
    uint8_t *data = malloc(size-32); assert(data);
    exact(client_fd, &client_cipher, data, size-32);
    plist_t root = NULL; plist_from_bin((char *)data,size-32,&root); free(data); assert(root);
    char *bytes = NULL; uint64_t length = 0;
    plist_get_data_val(plist_dict_get_item(plist_dict_get_item(root,"params"),"data"), &bytes,&length);
    plist_free(root); assert(bytes);
    const uint8_t *cursor = (uint8_t *)bytes; size_t remaining = length; uint64_t n;
    assert(!mrp_varint(&cursor,&remaining,&n) && n == remaining);
    Mrp__Protocol *message = mrp__protocol__unpack(NULL,n,cursor); free(bytes); assert(message);
    return message;
  }
}
static void *source_event(void *arg) {
  int fd = *(int *)arg;
  pair_cipher_bundle cipher = {0};
  cipher.cipher_ctx = pair_cipher_new(PAIR_CLIENT_HOMEKIT_NORMAL,1,secret,sizeof(secret),"");
  assert(cipher.cipher_ctx);
  for (unsigned request_index = 0; request_index < 2; request_index++) {
  uint8_t packet[4096]; size_t used = 0; char *end = NULL;
  do {
    ssize_t n = read_encrypted(fd,&cipher,packet+used,sizeof(packet)-used-1);
    assert(n > 0); used += n; packet[used] = 0;
    end = strstr((char *)packet,"\r\n\r\n");
  } while (!end);
  unsigned length = 0;
  assert(sscanf(strstr((char *)packet,"Content-Length:"),"Content-Length: %u",&length) == 1);
  size_t offset = end+4-(char *)packet;
  while (used < offset+length) {
    ssize_t n = read_encrypted(fd,&cipher,packet+used,sizeof(packet)-used);
    assert(n > 0); used += n;
  }
  plist_t command = NULL; plist_from_bin((char *)packet+offset,length,&command); assert(command);
  char *number = NULL;
  plist_get_string_val(plist_dict_get_item(command,"modernMediaRemoteCommand"),&number);
  assert(number && !strcmp(number,"24")); free(number);
  double position = 0;
  plist_get_real_val(plist_dict_get_item(plist_dict_get_item(command,"params"),
                    "kMRMediaRemoteOptionPlaybackPosition"),&position);
  assert(position == 30.5);
  plist_free(command);
  const char *response = "RTSP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n";
  assert(write_encrypted(fd,&cipher,response,strlen(response)) == (ssize_t)strlen(response));
  }
  pair_cipher_free(cipher.cipher_ctx);
  free(cipher.plaintext_read_buffer.data); free(cipher.encrypted_read_buffer.data);
  return NULL;
}

static void *event_controller(void *arg) {
  int fd = *(int *)arg;
  pair_cipher_bundle cipher = {0};
  cipher.cipher_ctx = pair_cipher_new(PAIR_CLIENT_HOMEKIT_NORMAL,1,secret,sizeof(secret),"");
  assert(cipher.cipher_ctx);
  for (unsigned message_index = 0; message_index < 3; message_index++) {
    uint8_t packet[4096]; size_t used = 0; char *end = NULL;
    do {
      ssize_t n = read_encrypted(fd,&cipher,packet+used,sizeof(packet)-used-1);
      assert(n > 0);used += n;packet[used]=0;end=strstr((char *)packet,"\r\n\r\n");
    } while (!end);
    assert(!memcmp(packet,"POST /command RTSP/1.0",21));
    unsigned length = 0;
    assert(sscanf(strstr((char *)packet,"Content-Length:"),"Content-Length: %u",&length) == 1);
    size_t offset = end+4-(char *)packet;assert(offset+length < sizeof(packet));
    while (used < offset+length) {
      ssize_t n=read_encrypted(fd,&cipher,packet+used,sizeof(packet)-used);
      assert(n>0);used+=n;
    }
    plist_t command = NULL;plist_from_bin((char *)packet+offset,length,&command);assert(command);
    plist_t params=plist_dict_get_item(command,"params");
    if (message_index == 0) {
      plist_t list=plist_dict_get_item(params,"mrSupportedCommandsFromSender");
      assert(plist_array_get_size(list)==1);
      char *data=NULL;uint64_t size=0;
      plist_get_data_val(plist_array_get_item(list,0),&data,&size);
      plist_t cap=NULL;plist_from_bin(data,size,&cap);free(data);assert(cap);
      uint64_t number=0;plist_get_uint_val(plist_dict_get_item(cap,"kCommandInfoCommandKey"),&number);
      assert(number==24);plist_free(cap);
    } else if (message_index == 1) {
      char *artist=NULL;
      plist_get_string_val(plist_dict_get_item(plist_dict_get_item(params,"params"),
                           "kMRMediaRemoteNowPlayingInfoArtist"),&artist);
      assert(artist && !strcmp(artist,"Live artist"));free(artist);
    } else {
      uint64_t state=0;plist_get_uint_val(plist_dict_get_item(params,"mrPlaybackState"),&state);
      assert(state==1);
    }
    plist_free(command);
    const char *response="RTSP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n";
    assert(write_encrypted(fd,&cipher,response,strlen(response))==(ssize_t)strlen(response));
  }
  pair_cipher_free(cipher.cipher_ctx);free(cipher.plaintext_read_buffer.data);free(cipher.encrypted_read_buffer.data);
  return NULL;
}
static void enable_seek(rtsp_conn_info *source) {
  plist_t record = plist_new_dict();
  plist_dict_set_item(record,"kCommandInfoCommandKey",plist_new_uint(24));
  plist_dict_set_item(record,"kCommandInfoEnabledKey",plist_new_bool(1));
  char *data = NULL; uint32_t size = 0; plist_to_bin(record,&data,&size); plist_free(record);
  plist_t list = plist_new_array(); plist_array_append_item(list,plist_new_data(data,size)); free(data);
  plist_t command = plist_new_dict(),params = plist_new_dict();
  plist_dict_set_item(params,"mrSupportedCommandsFromSender",list);
  plist_dict_set_item(command,"type",plist_new_string("updateMRSupportedCommands"));
  plist_dict_set_item(command,"params",params);
  ap2_mrp_observe(source,command); plist_free(command);
}
int main(void) {
  assert(sodium_init() >= 0); assert(gcry_check_version(NULL));
  config.service_name = "Test receiver";
  config.airplay_pi = "11111111-2222-3333-4444-555555555555";
  rtsp_conn_info *controller = calloc(1,sizeof(*controller)); assert(controller);
  controller->airplay_stream_category = remote_control_stream;
  controller->airplay_type = ap_2;
  controller->connection_ip_family = AF_INET;
  strcpy(controller->self_ip_string,"127.0.0.1");
  plist_t setup = plist_new_dict(), streams = plist_new_array(), stream = plist_new_dict();
  plist_dict_set_item(stream,"type",plist_new_uint(130));
  plist_dict_set_item(stream,"seed",plist_new_uint(123));
  plist_array_append_item(streams,stream);plist_dict_set_item(setup,"streams",streams);
  rtsp_message *setup_request = msg_init(), *setup_response = msg_init();
  strcpy(setup_request->path,"/stream");
  msg_add_header(setup_request,"Content-Type","application/x-apple-binary-plist");
  plist_to_bin(setup,&setup_request->content,&setup_request->contentlength);
  plist_free(setup);
  handle_setup_2(controller,setup_request,setup_response);
  assert(setup_response->respcode == 470 && controller->data_socket == 0);
  msg_free(&setup_response);setup_response = msg_init();
  controller->pair_setup_result = calloc(1,sizeof(*controller->pair_setup_result));
  assert(controller->pair_setup_result);
  memcpy(controller->pair_setup_result->shared_secret,secret,sizeof(secret));
  controller->pair_setup_result->shared_secret_len = sizeof(secret);
  controller->ap2_pairing_context.control_cipher_bundle.is_encrypted = 1;
  handle_setup_2(controller,setup_request,setup_response);
  assert(setup_response->respcode == 200 && controller->local_data_port && controller->mrp_thread);
  pthread_t *original_thread = controller->mrp_thread;
  msg_free(&setup_response);setup_response = msg_init();
  handle_setup_2(controller,setup_request,setup_response);
  assert(setup_response->respcode == 200 && controller->mrp_thread == original_thread);
  plist_t repeated_response = NULL;
  plist_from_bin(setup_response->content,setup_response->contentlength,&repeated_response);
  uint64_t repeated_port = 0;
  plist_get_uint_val(plist_dict_get_item(plist_array_get_item(plist_dict_get_item(repeated_response,
                     "streams"),0),"dataPort"),&repeated_port);
  assert(repeated_port == controller->local_data_port);plist_free(repeated_response);
  msg_free(&setup_response);msg_free(&setup_request);
  struct sockaddr_in address = {.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK),
                               .sin_port=htons(controller->local_data_port)};
  client_fd = socket(AF_INET,SOCK_STREAM,0); assert(client_fd >= 0);
  assert(!connect(client_fd,(struct sockaddr *)&address,sizeof(address)));
  struct timeval timeout = {.tv_sec=4};
  setsockopt(client_fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
  client_cipher.cipher_ctx = pair_cipher_new(PAIR_CLIENT_HOMEKIT_NORMAL,2,secret,sizeof(secret),"123");
  assert(client_cipher.cipher_ctx);
  Mrp__Protocol request = MRP__PROTOCOL__INIT;
  request.has_type = 1; request.type = 15; request.identifier = "device-request";
  send_request(&request);
  Mrp__Protocol *reply = receive_message();
  assert(reply->type == 15 && !strcmp(reply->identifier,"device-request"));
  assert(!strcmp(reply->device_info->name,"Test receiver")); mrp__protocol__free_unpacked(reply,NULL);
  Mrp__Updates updates = MRP__UPDATES__INIT;
  updates.has_now_playing = 1; updates.now_playing = 1;
  updates.has_artwork = 1; updates.artwork = 1;
  request.type = 16; request.updates = &updates; request.identifier = "subscribe";
  send_request(&request);
  reply = receive_message(); assert(reply->type == 16); mrp__protocol__free_unpacked(reply,NULL);
  reply = receive_message(); assert(reply->type == 46); mrp__protocol__free_unpacked(reply,NULL);
  reply = receive_message(); assert(reply->type == 47); mrp__protocol__free_unpacked(reply,NULL);
  reply = receive_message(); assert(reply->type == 4 && !reply->state->queue->n_items);
  mrp__protocol__free_unpacked(reply,NULL);
  request.updates = NULL;
  rtsp_conn_info *source = calloc(1,sizeof(*source)); assert(source);
  source->connection_number = 77; source->airplay_type = ap_2;
  source->airplay_gid = "11111111-2222-3333-4444-555555555555";
  pthread_mutex_init(&source->event_sender_mutex,NULL);
  principal_conn = source;
  plist_t info_command = plist_new_dict(), info_params = plist_new_dict(), info = plist_new_dict();
  plist_dict_set_item(info_command,"type",plist_new_string("updateMRNowPlayingInfo"));
  plist_dict_set_item(info_params,"type",plist_new_string("npi-text"));
  plist_dict_set_item(info_params,"mergePolicy",plist_new_string("replace"));
  plist_dict_set_item(info,"kMRMediaRemoteNowPlayingInfoTitle",plist_new_string("Live track"));
  plist_dict_set_item(info,"kMRMediaRemoteNowPlayingInfoArtist",plist_new_string("Live artist"));
  plist_dict_set_item(info,"kMRMediaRemoteNowPlayingInfoPlaybackRate",plist_new_real(1.0));
  plist_dict_set_item(info,"kMRMediaRemoteNowPlayingInfoArtworkData",plist_new_data("image",5));
  plist_dict_set_item(info_params,"params",info);
  plist_dict_set_item(info_command,"params",info_params);
  ap2_mrp_observe(source,info_command);plist_free(info_command);
  reply = receive_message();
  assert(reply->type == 4 && !strcmp(reply->state->now_playing->artist,"Live artist"));
  assert(reply->state->queue->items[0]->artwork.len == 5);
  mrp__protocol__free_unpacked(reply,NULL);
  enable_seek(source);
  reply = receive_message(); assert(reply->type == 4 && reply->state->supported_commands->n_commands == 1);
  assert(reply->state->playback_state == 1);
  mrp__protocol__free_unpacked(reply,NULL);
  // Event-only controller: no type-130 setup, plaintext protected by actual HAP encryption.
  int event_client_sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,event_client_sockets));
  controller->event_channel_fd=event_client_sockets[0];
  controller->ap2_pairing_context.event_cipher_bundle.cipher_ctx=
    pair_cipher_new(PAIR_SERVER_HOMEKIT,4,secret,sizeof(secret),"");
  assert(controller->ap2_pairing_context.event_cipher_bundle.cipher_ctx);
  pthread_mutex_init(&controller->event_sender_mutex,NULL);
  pthread_t event_client;assert(!pthread_create(&event_client,NULL,event_controller,&event_client_sockets[1]));
  uint64_t event_revision=0;
  assert(!ap2_mrp_send_event_state(controller,&event_revision,1));
  assert(!ap2_mrp_send_event_state(controller,&event_revision,0));
  pthread_join(event_client,NULL);
  close(event_client_sockets[0]);close(event_client_sockets[1]);
  controller->event_channel_fd=0;
  pair_cipher_free(controller->ap2_pairing_context.event_cipher_bundle.cipher_ctx);
  free(controller->ap2_pairing_context.event_cipher_bundle.plaintext_read_buffer.data);
  free(controller->ap2_pairing_context.event_cipher_bundle.encrypted_read_buffer.data);
  pthread_mutex_destroy(&controller->event_sender_mutex);
  int events[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,events));
  source->event_channel_fd = events[0];
  source->ap2_pairing_context.event_cipher_bundle.cipher_ctx =
    pair_cipher_new(PAIR_SERVER_HOMEKIT,4,secret,sizeof(secret),"");
  assert(source->ap2_pairing_context.event_cipher_bundle.cipher_ctx);
  pthread_t event_thread; assert(!pthread_create(&event_thread,NULL,source_event,&events[1]));
  Mrp__SendCommand send = MRP__SEND_COMMAND__INIT;
  Mrp__Options options = MRP__OPTIONS__INIT;
  send.has_command = 1; send.command = 45; send.options = &options;
  options.has_position = 1; options.position = 30.5;
  request.type = 1; request.send_command = &send; request.identifier = "seek";
  send_request(&request); reply = receive_message();
  assert(reply->type == 2 && reply->command_result->error == 0 && !strcmp(reply->identifier,"seek"));
  mrp__protocol__free_unpacked(reply,NULL);
  // Exercise the actual RTSP /command handler for event-channel control.
  plist_t event_command=plist_new_dict(), event_params=plist_new_dict();
  plist_dict_set_item(event_command,"type",plist_new_string("sendMediaRemoteCommand"));
  plist_dict_set_item(event_command,"modernMediaRemoteCommand",plist_new_string("24"));
  plist_dict_set_item(event_params,"kMRMediaRemoteOptionPlaybackPosition",plist_new_real(30.5));
  plist_dict_set_item(event_command,"params",event_params);
  rtsp_message *event_request=msg_init(), *event_response=msg_init();
  msg_add_header(event_request,"Content-Type","application/x-apple-binary-plist");
  plist_to_bin(event_command,&event_request->content,&event_request->contentlength);plist_free(event_command);
  controller->ap2_pairing_context.control_cipher_bundle.is_encrypted=0;
  handle_command(controller,event_request,event_response);assert(event_response->respcode==470);
  controller->ap2_pairing_context.control_cipher_bundle.is_encrypted=1;
  handle_command(controller,event_request,event_response);assert(event_response->respcode==200);
  msg_free(&event_request);msg_free(&event_response);pthread_join(event_thread,NULL);
  options.position = -1; send_request(&request); reply = receive_message();
  assert(reply->command_result->error == 7); mrp__protocol__free_unpacked(reply,NULL);
  send.command = 1; send_request(&request); reply = receive_message();
  assert(reply->command_result->error == 10); mrp__protocol__free_unpacked(reply,NULL);
  mrp_clear(77); reply = receive_message(); assert(reply->type == 4 && !reply->state->supported_commands->n_commands);
  mrp__protocol__free_unpacked(reply,NULL);
  close(client_fd); pthread_join(*controller->mrp_thread,NULL); free(controller->mrp_thread); close(controller->data_socket);
  pair_cipher_free(client_cipher.cipher_ctx);
  free(client_cipher.plaintext_read_buffer.data);free(client_cipher.encrypted_read_buffer.data);
  pair_cipher_free(controller->ap2_pairing_context.data_cipher_bundle.cipher_ctx);
  free(controller->ap2_pairing_context.data_cipher_bundle.plaintext_read_buffer.data);
  free(controller->ap2_pairing_context.data_cipher_bundle.encrypted_read_buffer.data);
  close(events[0]);close(events[1]);pair_cipher_free(source->ap2_pairing_context.event_cipher_bundle.cipher_ctx);
  free(source->ap2_pairing_context.event_cipher_bundle.plaintext_read_buffer.data);
  free(source->ap2_pairing_context.event_cipher_bundle.encrypted_read_buffer.data);
  pthread_mutex_destroy(&source->event_sender_mutex);principal_conn=NULL;free(source);free(controller->pair_setup_result);
  free(controller->ap2_pairing_context.data_cipher_bundle.description);free(controller);
  puts("Encrypted MRP channel tests passed: data and event transports, authenticated RTSP controls, rate-derived playback state, live metadata, source seek, command errors and disconnect.");
  return 0;
}
