/* Sonic Loader — EchoStretch/ps5-app-dumper bridge. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <microhttpd.h>

#include "dumper.h"
#include "sys.h"
#include "third_party/cJSON.h"
#include "websrv.h"


static enum MHD_Result
serve_buffer(struct MHD_Connection *conn, unsigned int status,
             const char *mime, void *data, size_t size, int free_after) {
  enum MHD_Result ret = MHD_NO;
  struct MHD_Response *resp;
  enum MHD_ResponseMemoryMode mode = free_after ? MHD_RESPMEM_MUST_FREE
                                                : MHD_RESPMEM_PERSISTENT;
  if((resp=MHD_create_response_from_buffer(size, data, mode))) {
    if(mime) MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, mime);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CACHE_CONTROL, "no-cache");
    ret = websrv_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
  } else if(free_after) {
    free(data);
  }
  return ret;
}


static enum MHD_Result
serve_json(struct MHD_Connection *conn, unsigned int status, cJSON *o) {
  char *txt = cJSON_PrintUnformatted(o);
  if(!txt) return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                               "application/json",
                               "{\"error\":\"alloc\"}", 17, 0);
  return serve_buffer(conn, status, "application/json", txt, strlen(txt), 1);
}


static enum MHD_Result
run_request(struct MHD_Connection *conn) {
  if(sys_spawn_app_dumper() != 0) {
    cJSON *e = cJSON_CreateObject();
    cJSON_AddBoolToObject(e,   "ok", 0);
    cJSON_AddStringToObject(e, "error",
        "spawn failed — check the system log");
    enum MHD_Result ret = serve_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, e);
    cJSON_Delete(e);
    return ret;
  }
  cJSON *r = cJSON_CreateObject();
  cJSON_AddBoolToObject(r,   "ok", 1);
  cJSON_AddStringToObject(r, "payload", "ps5-app-dumper.elf");
  cJSON_AddStringToObject(r, "note",
      "Spawned. The dumper auto-detects the first writable USB drive "
      "and walks /mnt/sandbox/pfsmnt copying every mounted app/patch "
      "into <usb>/PS5/<TITLE_ID>/. Watch the system notifications.");
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}


enum MHD_Result
dumper_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/dumper/run")) return run_request(conn);

  cJSON *err = cJSON_CreateObject();
  cJSON_AddBoolToObject(err, "ok", 0);
  cJSON_AddStringToObject(err, "error", "no such endpoint");
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_NOT_FOUND, err);
  cJSON_Delete(err);
  return ret;
}
