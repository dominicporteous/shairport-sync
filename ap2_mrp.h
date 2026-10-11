#pragma once
#include "player.h"
#include "rtsp.h"
void *ap2_mrp_receiver(void *arg);
void ap2_mrp_observe(rtsp_conn_info *conn, plist_t command);
// Send snapshots to an authenticated event-only remote controller.
int ap2_mrp_send_event_state(rtsp_conn_info *conn, uint64_t *revision, int force);
// Return nonzero when a controller's /command has been handled.
int ap2_mrp_handle_event_command(rtsp_conn_info *conn, plist_t command, rtsp_message *response);
