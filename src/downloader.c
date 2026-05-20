/* Sonic Loader - exFAT / FFPKG downloader payload bridge.

   Before injection, sys.c patches URL / destination / UDP port into a
   copy of exFAT_FFPKG.elf. The payload downloads in its own process and
   posts small JSON progress packets back to 127.0.0.1:9876. */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>

#include <microhttpd.h>

#include "downloader.h"
#include "sys.h"
#include "third_party/cJSON.h"
#include "websrv.h"

#define DOWNLOADER_UDP_PORT 9876

typedef enum {
  DL_STATE_IDLE = 0,
  DL_STATE_STARTING,
  DL_STATE_RUNNING,
  DL_STATE_DONE,
  DL_STATE_ERROR,
  DL_STATE_CANCELLED,
} dl_state_t;

typedef struct {
  dl_state_t state;
  char type[16];
  char url[1024];
  char dest[512];
  char status[96];
  char error[192];
  int percent;
  double speed;
  long long downloaded;
  long long total;
  time_t started_at;
  time_t updated_at;
  time_t finished_at;
} downloader_status_t;

static pthread_mutex_t g_dl_lock = PTHREAD_MUTEX_INITIALIZER;
static downloader_status_t g_dl;
static int g_listener_started = 0;

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
serve_json(struct MHD_Connection *conn, unsigned int status, cJSON *obj) {
  char *txt = cJSON_PrintUnformatted(obj);
  if(!txt) {
    return serve_buffer(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                        "application/json", "{\"error\":\"alloc\"}", 17, 0);
  }
  return serve_buffer(conn, status, "application/json", txt, strlen(txt), 1);
}

static enum MHD_Result
serve_error(struct MHD_Connection *conn, unsigned int status,
            const char *msg) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddBoolToObject(o, "ok", 0);
  cJSON_AddStringToObject(o, "error", msg ? msg : "error");
  enum MHD_Result ret = serve_json(conn, status, o);
  cJSON_Delete(o);
  return ret;
}

static const char*
state_name(dl_state_t s) {
  switch(s) {
    case DL_STATE_STARTING:  return "starting";
    case DL_STATE_RUNNING:   return "running";
    case DL_STATE_DONE:      return "done";
    case DL_STATE_ERROR:     return "error";
    case DL_STATE_CANCELLED: return "cancelled";
    case DL_STATE_IDLE:
    default:                 return "idle";
  }
}

static int
valid_remote_url(const char *url) {
  if(!url || !*url || strlen(url) >= 1000) return 0;
  if(strncmp(url, "http://", 7) != 0 &&
     strncmp(url, "https://", 8) != 0) return 0;
  for(const unsigned char *p = (const unsigned char*)url; *p; p++) {
    if(*p < 0x20 || *p == 0x7f) return 0;
  }
  return 1;
}

static int
is_ffpkg_type(const char *type) {
  return type &&
    (!strcasecmp(type, "ffpkg") ||
     !strcasecmp(type, "fpkg")  ||
     !strcasecmp(type, "pkg"));
}

static void
sanitize_name(const char *in, char *out, size_t out_size,
              const char *fallback) {
  if(!out || out_size == 0) return;
  out[0] = 0;
  if(!in || !*in) in = fallback;

  const char *base = in;
  const char *slash1 = strrchr(base, '/');
  const char *slash2 = strrchr(base, '\\');
  if(slash1 && (!slash2 || slash1 > slash2)) base = slash1 + 1;
  if(slash2 && (!slash1 || slash2 > slash1)) base = slash2 + 1;

  size_t j = 0;
  for(size_t i = 0; base[i] && j + 1 < out_size; i++) {
    unsigned char c = (unsigned char)base[i];
    if(isalnum(c) || c == '.' || c == '_' || c == '-' || c == '+') {
      out[j++] = (char)c;
    } else {
      out[j++] = '_';
    }
  }
  out[j] = 0;

  if(!out[0] || out[0] == '.') {
    snprintf(out, out_size, "%s", fallback);
  }
  while(strstr(out, "..")) {
    char *p = strstr(out, "..");
    p[1] = '_';
  }
}

static void
basename_from_url(const char *url, char *out, size_t out_size,
                  const char *fallback) {
  char tmp[256];
  size_t n = 0;
  const char *end = url ? strpbrk(url, "?#") : NULL;
  if(!end && url) end = url + strlen(url);

  const char *base = url;
  if(url && end) {
    for(const char *p = url; p < end; p++) {
      if(*p == '/') base = p + 1;
    }
    while(base < end && n + 1 < sizeof(tmp)) tmp[n++] = *base++;
  }
  tmp[n] = 0;
  sanitize_name(tmp, out, out_size, fallback);
}

static int
has_ext_ci(const char *name, const char *ext) {
  size_t nn = name ? strlen(name) : 0;
  size_t en = ext ? strlen(ext) : 0;
  if(nn < en) return 0;
  return strcasecmp(name + nn - en, ext) == 0;
}

static void
ensure_extension(char *name, size_t name_size, const char *ext) {
  if(!name || !ext || has_ext_ci(name, ext)) return;
  size_t n = strlen(name), e = strlen(ext);
  if(n + e + 1 <= name_size) {
    memcpy(name + n, ext, e + 1);
  }
}

static void
ensure_game_download_dirs(const char *type) {
  mkdir("/data", 0777);
  if(is_ffpkg_type(type)) {
    mkdir("/data/sonic-loader", 0777);
    mkdir("/data/sonic-loader/pkgs", 0777);
    chmod("/data/sonic-loader/pkgs", 0777);
  } else {
    mkdir("/data/etaHEN", 0777);
    mkdir("/data/etaHEN/games", 0777);
    chmod("/data/etaHEN/games", 0777);
  }
}

static int
build_destination(const char *type, const char *url, const char *name,
                  char *dest, size_t dest_size) {
  char file[192];
  const int ffpkg = is_ffpkg_type(type);
  const char *fallback = ffpkg ? "download.pkg" : "download.exfat";

  if(name && *name) sanitize_name(name, file, sizeof(file), fallback);
  else              basename_from_url(url, file, sizeof(file), fallback);

  if(ffpkg) {
    if(!has_ext_ci(file, ".pkg") && !has_ext_ci(file, ".fpkg")) {
      ensure_extension(file, sizeof(file), ".pkg");
    }
    return snprintf(dest, dest_size, "/data/sonic-loader/pkgs/%s", file) <
           (int)dest_size ? 0 : -1;
  }

  ensure_extension(file, sizeof(file), ".exfat");
  return snprintf(dest, dest_size, "/data/etaHEN/games/%s", file) <
         (int)dest_size ? 0 : -1;
}

static void
copy_string(char *dst, size_t dst_size, const char *src) {
  if(!dst || dst_size == 0) return;
  if(!src) src = "";
  strncpy(dst, src, dst_size - 1);
  dst[dst_size - 1] = 0;
}

static void
apply_udp_packet(const char *packet) {
  cJSON *o = cJSON_Parse(packet);
  if(!o) return;

  cJSON *p  = cJSON_GetObjectItem(o, "p");
  cJSON *s  = cJSON_GetObjectItem(o, "s");
  cJSON *d  = cJSON_GetObjectItem(o, "d");
  cJSON *t  = cJSON_GetObjectItem(o, "t");
  cJSON *st = cJSON_GetObjectItem(o, "st");
  const char *status = cJSON_IsString(st) ? st->valuestring : "";
  time_t now = time(NULL);

  pthread_mutex_lock(&g_dl_lock);
  if(g_dl.state == DL_STATE_STARTING ||
     g_dl.state == DL_STATE_RUNNING) {
    if(cJSON_IsNumber(p)) {
      int pct = p->valueint;
      if(pct < 0) pct = 0;
      if(pct > 100) pct = 100;
      g_dl.percent = pct;
    }
    if(cJSON_IsNumber(s)) g_dl.speed = s->valuedouble;
    if(cJSON_IsNumber(d) && d->valuedouble >= 0) {
      g_dl.downloaded = (long long)d->valuedouble;
    }
    if(cJSON_IsNumber(t) && t->valuedouble >= 0) {
      g_dl.total = (long long)t->valuedouble;
    }
    if(status[0]) copy_string(g_dl.status, sizeof(g_dl.status), status);
    g_dl.updated_at = now;

    if(!strcmp(status, "completed")) {
      g_dl.state = DL_STATE_DONE;
      g_dl.percent = 100;
      if(g_dl.total <= 0 && g_dl.downloaded > 0) g_dl.total = g_dl.downloaded;
      if(g_dl.downloaded <= 0 && g_dl.total > 0) g_dl.downloaded = g_dl.total;
      g_dl.finished_at = now;
    } else if(!strncmp(status, "error", 5)) {
      g_dl.state = DL_STATE_ERROR;
      copy_string(g_dl.error, sizeof(g_dl.error), status);
      g_dl.finished_at = now;
    } else if(!strcmp(status, "cancelled")) {
      g_dl.state = DL_STATE_CANCELLED;
      g_dl.finished_at = now;
    } else if(!strcmp(status, "starting")) {
      g_dl.state = DL_STATE_STARTING;
    } else {
      g_dl.state = DL_STATE_RUNNING;
    }
  }
  pthread_mutex_unlock(&g_dl_lock);
  cJSON_Delete(o);
}

static void*
udp_listener_thread(void *arg) {
  int sock = (int)(intptr_t)arg;
  char buf[512];

  for(;;) {
    ssize_t n = recvfrom(sock, buf, sizeof(buf) - 1, 0, NULL, NULL);
    if(n <= 0) {
      if(errno == EINTR) continue;
      break;
    }
    buf[n] = 0;
    apply_udp_packet(buf);
  }
  return NULL;
}

static int
ensure_udp_listener_locked(void) {
  if(g_listener_started) return 0;

  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if(s < 0) return -1;

  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(DOWNLOADER_UDP_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if(bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    close(s);
    return -1;
  }

  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  if(pthread_create(&t, &a, udp_listener_thread,
                    (void*)(intptr_t)s) != 0) {
    pthread_attr_destroy(&a);
    close(s);
    return -1;
  }
  pthread_attr_destroy(&a);
  g_listener_started = 1;
  return 0;
}

static void
status_to_json_locked(cJSON *r) {
  time_t now = time(NULL);
  cJSON_AddBoolToObject(r,   "ok",         1);
  cJSON_AddStringToObject(r, "state",      state_name(g_dl.state));
  cJSON_AddStringToObject(r, "type",       g_dl.type);
  cJSON_AddStringToObject(r, "url",        g_dl.url);
  cJSON_AddStringToObject(r, "dest",       g_dl.dest);
  cJSON_AddStringToObject(r, "status",     g_dl.status);
  cJSON_AddNumberToObject(r, "percent",    g_dl.percent);
  cJSON_AddNumberToObject(r, "speed",      g_dl.speed);
  cJSON_AddNumberToObject(r, "downloaded", (double)g_dl.downloaded);
  cJSON_AddNumberToObject(r, "total",      (double)g_dl.total);
  cJSON_AddNumberToObject(r, "startedAt",  (double)g_dl.started_at);
  cJSON_AddNumberToObject(r, "updatedAt",  (double)g_dl.updated_at);
  cJSON_AddNumberToObject(r, "finishedAt", (double)g_dl.finished_at);
  if(g_dl.error[0]) cJSON_AddStringToObject(r, "error", g_dl.error);
  cJSON_AddBoolToObject(r, "stale",
    (g_dl.state == DL_STATE_STARTING || g_dl.state == DL_STATE_RUNNING) &&
    g_dl.updated_at > 0 && now - g_dl.updated_at > 20);
}

static enum MHD_Result
status_request(struct MHD_Connection *conn) {
  cJSON *r = cJSON_CreateObject();
  pthread_mutex_lock(&g_dl_lock);
  status_to_json_locked(r);
  pthread_mutex_unlock(&g_dl_lock);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}

static enum MHD_Result
start_request(struct MHD_Connection *conn) {
  const char *url = MHD_lookup_connection_value(conn,
                       MHD_GET_ARGUMENT_KIND, "url");
  const char *type_arg = MHD_lookup_connection_value(conn,
                            MHD_GET_ARGUMENT_KIND, "type");
  const char *name = MHD_lookup_connection_value(conn,
                        MHD_GET_ARGUMENT_KIND, "name");
  const char *type = (type_arg && *type_arg) ? type_arg : "exfat";

  if(!valid_remote_url(url)) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "url must start with http:// or https://");
  }
  if(strcasecmp(type, "exfat") != 0 && !is_ffpkg_type(type)) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "type must be exfat or ffpkg");
  }

  char dest[512];
  if(build_destination(type, url, name, dest, sizeof(dest)) != 0) {
    return serve_error(conn, MHD_HTTP_BAD_REQUEST,
                       "destination path is too long");
  }
  ensure_game_download_dirs(type);

  pthread_mutex_lock(&g_dl_lock);
  if(g_dl.state == DL_STATE_STARTING || g_dl.state == DL_STATE_RUNNING) {
    pthread_mutex_unlock(&g_dl_lock);
    return serve_error(conn, MHD_HTTP_CONFLICT,
                       "a download is already running");
  }
  if(ensure_udp_listener_locked() != 0) {
    pthread_mutex_unlock(&g_dl_lock);
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "could not bind UDP progress listener");
  }

  memset(&g_dl, 0, sizeof(g_dl));
  g_dl.state = DL_STATE_STARTING;
  g_dl.started_at = time(NULL);
  g_dl.updated_at = g_dl.started_at;
  copy_string(g_dl.type, sizeof(g_dl.type),
              is_ffpkg_type(type) ? "ffpkg" : "exfat");
  copy_string(g_dl.url, sizeof(g_dl.url), url);
  copy_string(g_dl.dest, sizeof(g_dl.dest), dest);
  copy_string(g_dl.status, sizeof(g_dl.status), "starting");
  pthread_mutex_unlock(&g_dl_lock);

  if(sys_spawn_exfat_ffpkg_downloader(url, dest, DOWNLOADER_UDP_PORT) != 0) {
    pthread_mutex_lock(&g_dl_lock);
    g_dl.state = DL_STATE_ERROR;
    g_dl.finished_at = time(NULL);
    copy_string(g_dl.status, sizeof(g_dl.status), "error:spawn");
    copy_string(g_dl.error, sizeof(g_dl.error),
                "failed to spawn exFAT_FFPKG.elf");
    pthread_mutex_unlock(&g_dl_lock);
    return serve_error(conn, MHD_HTTP_INTERNAL_SERVER_ERROR,
                       "failed to spawn downloader payload");
  }

  cJSON *r = cJSON_CreateObject();
  pthread_mutex_lock(&g_dl_lock);
  status_to_json_locked(r);
  pthread_mutex_unlock(&g_dl_lock);
  enum MHD_Result ret = serve_json(conn, MHD_HTTP_OK, r);
  cJSON_Delete(r);
  return ret;
}

enum MHD_Result
downloader_request(struct MHD_Connection *conn, const char *url) {
  if(!strcmp(url, "/api/downloader/start"))  return start_request(conn);
  if(!strcmp(url, "/api/downloader/status")) return status_request(conn);
  return serve_error(conn, MHD_HTTP_NOT_FOUND, "no such endpoint");
}
