/* Real non-transient SRP setup and signature-verified HomeKit pair verification.
 * All keys are generated in this process and are never printed or persisted. */
#include "config.h"
#include "common.h"
#include "rtsp.h"
#include "pair_ap/pair-internal.h"
#include "utilities/rtsp_message_utilities.h"
#include <assert.h>
#include <gcrypt.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>

extern void handle_pair_setup(rtsp_conn_info *,rtsp_message *,rtsp_message *);
extern void handle_pair_verify(rtsp_conn_info *,rtsp_message *,rtsp_message *);
extern void handle_pair_remove(rtsp_conn_info *,rtsp_message *,rtsp_message *);
extern void handle_configure(rtsp_conn_info *,rtsp_message *,rtsp_message *);
extern ssize_t read_from_rtsp_connection(rtsp_conn_info *,void *,size_t);

static rtsp_message *request(uint8_t *body, size_t size, const char *version) {
  rtsp_message *message=msg_init();message->content=(char *)body;message->contentlength=size;
  msg_add_header(message,"X-Apple-HKP",(char *)version);
  return message;
}
static void cleanup(rtsp_conn_info *conn) {
  pair_cipher_free(conn->ap2_pairing_context.control_cipher_bundle.cipher_ctx);
  pair_cipher_free(conn->ap2_pairing_context.event_cipher_bundle.cipher_ctx);
  free(conn->ap2_pairing_context.control_cipher_bundle.description);
  free(conn->ap2_pairing_context.event_cipher_bundle.description);
  free(conn->ap2_pairing_context.control_cipher_bundle.plaintext_read_buffer.data);
  free(conn->ap2_pairing_context.control_cipher_bundle.encrypted_read_buffer.data);
  pair_verify_free(conn->ap2_pairing_context.verify_ctx);
  pair_setup_free(conn->ap2_pairing_context.setup_ctx);
  memset(conn,0,sizeof(*conn));
}
static void verify(const char *keys, const char *client_id, const char *version, int accepted) {
  struct pair_verify_context *client=pair_verify_new(PAIR_CLIENT_HOMEKIT_NORMAL,keys,NULL,NULL,client_id);
  assert(client);
  rtsp_conn_info server={0};size_t size=0;
  uint8_t *body=pair_verify_request1(&size,client);assert(body);
  rtsp_message *req=request(body,size,version), *resp=msg_init();resp->respcode=200;
  handle_pair_verify(&server,req,resp);
  assert(resp->respcode==200 && !server.ap2_pairing_context.control_cipher_bundle.cipher_ctx);
  // M2 must use the /configure identity for HomeKit 6/7, while retaining the
  // previously advertised public key. AirPlay HAP 3 retains its MAC identity.
  const char *identity=!strcmp(version,"3")?config.airplay_device_id:config.airplay_pi;
  assert(!strcmp(server.ap2_pairing_context.verify_ctx->vctx.server.device_id,identity));
  assert(!memcmp(server.ap2_pairing_context.verify_ctx->vctx.server.server_public_key,
                 config.airplay_pk,32));
  assert(!pair_verify_response1(client,(uint8_t *)resp->content,resp->contentlength));
  msg_free(&req);msg_free(&resp);
  body=pair_verify_request2(&size,client);assert(body);
  req=request(body,size,version);resp=msg_init();resp->respcode=200;
  handle_pair_verify(&server,req,resp);
  int result=pair_verify_response2(client,(uint8_t *)resp->content,resp->contentlength);
  if (accepted) {
    assert(result==0 && server.pair_setup_result && server.pair_setup_result->shared_secret_len==32);
    assert(server.ap2_pairing_context.control_cipher_bundle.cipher_ctx);
    assert(server.ap2_pairing_context.event_cipher_bundle.cipher_ctx);
    // M4 remains plaintext; the first subsequent request activates encryption.
    assert(!server.ap2_pairing_context.control_cipher_bundle.is_encrypted);
    struct pair_result *client_result=NULL;assert(!pair_verify_result(&client_result,client));
    assert(!memcmp(client_result->shared_secret,server.pair_setup_result->shared_secret,32));
    pair_cipher_bundle cipher={0};
    cipher.cipher_ctx=pair_cipher_new(PAIR_CLIENT_HOMEKIT_NORMAL,0,
                        client_result->shared_secret,client_result->shared_secret_len,"");
    assert(cipher.cipher_ctx);
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    const char *next_request="GET /info RTSP/1.0\r\nCSeq: 3\r\n\r\n";
    assert(write_encrypted(sockets[0],&cipher,next_request,strlen(next_request))==(ssize_t)strlen(next_request));
    char decoded[256];
    server.fd=sockets[1];
    ssize_t length=read_from_rtsp_connection(&server,decoded,sizeof(decoded));
    assert(length==(ssize_t)strlen(next_request) && !memcmp(decoded,next_request,length));
    assert(server.ap2_pairing_context.control_cipher_bundle.is_encrypted);
    close(sockets[0]);close(sockets[1]);pair_cipher_free(cipher.cipher_ctx);
  } else {
    assert(result<0);
    assert(!server.pair_setup_result && !server.ap2_pairing_context.control_cipher_bundle.cipher_ctx);
  }
  msg_free(&req);msg_free(&resp);pair_verify_free(client);cleanup(&server);
}
static char *setup(const char *version,const char *client_id) {
  struct pair_setup_context *client=pair_setup_new(PAIR_CLIENT_HOMEKIT_NORMAL,"3939",NULL,NULL,client_id);
  assert(client);rtsp_conn_info server={0};
  for (unsigned step=1;step<=3;step++) {
    size_t size=0;uint8_t *body=step==1?pair_setup_request1(&size,client):
                   step==2?pair_setup_request2(&size,client):pair_setup_request3(&size,client);
    assert(body);rtsp_message *req=request(body,size,version),*resp=msg_init();resp->respcode=200;
    handle_pair_setup(&server,req,resp);assert(resp->respcode==200 && resp->content);
    int result=step==1?pair_setup_response1(client,(uint8_t *)resp->content,resp->contentlength):
               step==2?pair_setup_response2(client,(uint8_t *)resp->content,resp->contentlength):
                       pair_setup_response3(client,(uint8_t *)resp->content,resp->contentlength);
    assert(!result);msg_free(&req);msg_free(&resp);
  }
  const char *keys=NULL;struct pair_result *result=NULL;
  assert(!pair_setup_result(&keys,&result,client));
  assert(!strcmp(result->device_id,!strcmp(version,"3")?config.airplay_device_id:config.airplay_pi));
  assert(!memcmp(result->server_public_key,config.airplay_pk,32));
  char *copy=strdup(keys);assert(copy);pair_setup_free(client);cleanup(&server);return copy;
}
static void configure_home(int enabled) {
  plist_t message=plist_new_dict(), dictionary=plist_new_dict();
  plist_dict_set_item(dictionary,"Enable_HK_Access_Control",plist_new_bool(enabled));
  plist_dict_set_item(message,"ConfigurationDictionary",dictionary);
  rtsp_message *req=msg_init(),*resp=msg_init();resp->respcode=200;
  strcpy(req->path,"/configure");plist_to_bin(message,&req->content,&req->contentlength);
  rtsp_conn_info server={0};handle_configure(&server,req,resp);
  assert(resp->respcode==200 && config.enable_HK_Access_Control==enabled);
  assert(!!(config.airplay_statusflags & (1<<10))==enabled);
  plist_free(message);msg_free(&req);msg_free(&resp);
}
static void fresh_process(const char *executable,const char *mode,const char *file,const char *keys,
                          const char *client,const char *version,int accepted,int home_enabled) {
  int pipefd[2];assert(!pipe(pipefd));pid_t child=fork();assert(child>=0);
  if (!child) {
    close(pipefd[1]);assert(dup2(pipefd[0],STDIN_FILENO)>=0);close(pipefd[0]);
    execl(executable,executable,mode,file,client,version,accepted?"1":"0",home_enabled?"1":"0",NULL);
    _exit(127);
  }
  close(pipefd[0]);assert(write(pipefd[1],keys,strlen(keys))==(ssize_t)strlen(keys));close(pipefd[1]);
  int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
}
int main(int argc,char **argv) {
  assert(sodium_init()>=0 && gcry_check_version(NULL));
  gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
  config.password="3939";
  config.airplay_device_id="72:02:0a:74:f8:2f";
  config.airplay_pi="11111111-2222-3333-4444-555555555555";
  config.firmware_version=config.model=config.srcvers=config.pk_string=config.airplay_fex="test";
  config.airplay_psi=config.airplay_pi;config.service_name="test";
  pair_public_key_get(PAIR_SERVER_HOMEKIT,config.airplay_pk,config.airplay_device_id);
  if (argc==7) {
    config.homekit_pairing_file=argv[2];
    char keys[193]={0};size_t size=0;ssize_t count;
    while ((count=read(STDIN_FILENO,keys+size,sizeof(keys)-1-size))>0) size+=count;
    assert(size==192);
    if (!strcmp(argv[1],"foreign")) config.airplay_pi="another-receiver";
    if (!strcmp(argv[1],"invalid") || !strcmp(argv[1],"foreign")) {
      assert(rtsp_pairings_init()<0);return 0;
    }
    assert(!rtsp_pairings_init());
    assert(config.enable_HK_Access_Control==atoi(argv[6]));
    assert(!!(config.airplay_statusflags & (1<<10))==atoi(argv[6]));
    verify(keys,argv[3],argv[4],atoi(argv[5]));return 0;
  }
  char directory[]="/tmp/shairport-homekit-test-XXXXXX";assert(mkdtemp(directory));
  char file[256];snprintf(file,sizeof(file),"%s/pairings.plist",directory);
  config.homekit_pairing_file=file;assert(!rtsp_pairings_init());
  configure_home(1);
  const char *home_client="test-home-controller";
  char *keys=setup("6",home_client);
  verify(keys,home_client,"6",1);verify(keys,home_client,"7",1);
  struct stat file_info;assert(!stat(file,&file_info) && (file_info.st_mode&0777)==0600);
  fresh_process(argv[0],"verify",file,keys,home_client,"7",1,1);
  fresh_process(argv[0],"foreign",file,keys,home_client,"7",0,1);
  verify(keys,"unknown-home-controller","7",0);
  uint8_t unknown_public[32],unknown_private[64];crypto_sign_keypair(unknown_public,unknown_private);
  char forged_keys[193];const char hex[]="0123456789abcdef";
  for (unsigned i=0;i<96;i++) {
    uint8_t byte=i<64?unknown_private[i]:config.airplay_pk[i-64];
    forged_keys[i*2]=hex[byte>>4];forged_keys[i*2+1]=hex[byte&15];
  }
  forged_keys[192]=0;
  verify(forged_keys,home_client,"7",0);
  sodium_memzero(forged_keys,sizeof(forged_keys));sodium_memzero(unknown_private,sizeof(unknown_private));
  // Removal must survive a restart and reject the previously enrolled controller.
  size_t id_size=strlen(home_client);uint8_t *removal=malloc(id_size+5);assert(removal);
  removal[0]=6;removal[1]=1;removal[2]=1;removal[3]=1;removal[4]=id_size;
  memcpy(removal+5,home_client,id_size);
  rtsp_message *req=request(removal,id_size+5,"7"),*resp=msg_init();resp->respcode=200;
  rtsp_conn_info server={0};handle_pair_remove(&server,req,resp);assert(resp->respcode==200);
  msg_free(&req);msg_free(&resp);
  fresh_process(argv[0],"verify",file,keys,home_client,"7",0,1);
  configure_home(0);
  fresh_process(argv[0],"verify",file,keys,home_client,"7",0,0);
  free(keys);
  keys=setup("3","test-airplay-controller");verify(keys,"test-airplay-controller","3",1);
  FILE *invalid=fopen(file,"w");assert(invalid);assert(fputs("invalid plist",invalid)>=0);assert(!fclose(invalid));
  fresh_process(argv[0],"invalid",file,keys,home_client,"7",0,0);free(keys);
  assert(!unlink(file) && !rmdir(directory));
  puts("HomeKit pairing tests passed: real SRP setup, UUID identity, existing public key, M1-M4 verification, post-M4 encryption, unknown peer and forged signature rejection, legacy HAP identity, persistent enrolment/removal/Home flag, foreign and corrupt state rejection.");
  return 0;
}
