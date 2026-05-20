
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <curl/curl.h>


#define TARGET_PORT   9876

#define MAX_PATH      512
#define UDP_BUF_SIZE  256
#define SOCK_RCVBUF     (8 * 1024 * 1024)
#define FILE_WRITE_BUF  (4 * 1024 * 1024)
#define PATCH_URL_MARKER  "SL_EXFAT_FFPKG_URL="
#define PATCH_DEST_MARKER "SL_EXFAT_FFPKG_DEST="
#define PATCH_PORT_MARKER "SL_EXFAT_FFPKG_PORT="

static char g_patch_url[1024]  = PATCH_URL_MARKER;
static char g_patch_dest[512]  = PATCH_DEST_MARKER;
static char g_patch_port[32]   = PATCH_PORT_MARKER "9876";

typedef struct {
    int  type;
    int  req_id;
    int  priority;
    int  msg_id;
    int  target_id;
    int  user_id;
    int  unk1;
    int  unk2;
    int  app_id;
    int  error_num;
    int  unk3;
    char use_icon_image_uri;
    char message[1024];
    char icon_uri[1024];
    char unk4[1024];
} OrbisNotificationRequest;

extern int sceKernelSendNotificationRequest(
    int device, OrbisNotificationRequest *req, size_t size, int blocking);

static void ps5_notify(const char *fmt, ...)
{
    OrbisNotificationRequest req;
    memset(&req, 0, sizeof(req));
    req.type = 0;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(req.message, sizeof(req.message), fmt, ap);
    va_end(ap);

    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

typedef struct {
    int                udp_sock;
    struct sockaddr_in udp_addr;
    CURL              *curl;
    curl_off_t         resume_offset;
    int                cancelled;
    time_t             last_udp_time;
    int                last_notif_pct;
} dl_ctx_t;


static long get_file_size(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) ? (long)st.st_size : -1L;
}

static void ensure_dir(const char *full_path)
{
    char dir[MAX_PATH];
    strncpy(dir, full_path, MAX_PATH - 1);
    dir[MAX_PATH - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        mkdir(dir, 0777);
    }
}

static const char *file_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const char *patched_value(const char *slot, size_t marker_len)
{
    const char *value = slot + marker_len;
    return value[0] ? value : NULL;
}

static int get_system_dns(char *out, size_t outsz)
{
    out[0] = '\0';
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return 0;

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "nameserver", 10) != 0) continue;
        char *ip = line + 10;
        while (*ip == ' ' || *ip == '\t') ip++;
        char *end = ip;
        while (*end && *end != '\r' && *end != '\n' && *end != ' ') end++;
        *end = '\0';
        if (strlen(ip) >= 7) {
            strncpy(out, ip, outsz - 1);
            out[outsz - 1] = '\0';
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static void fmt_size(char *buf, size_t bufsz, int64_t bytes)
{
    if      (bytes >= (int64_t)1073741824)
        snprintf(buf, bufsz, "%.2f GB", (double)bytes / 1073741824.0);
    else if (bytes >= 1048576)
        snprintf(buf, bufsz, "%.1f MB", (double)bytes / 1048576.0);
    else if (bytes >= 1024)
        snprintf(buf, bufsz, "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(buf, bufsz, "%lld B", (long long)bytes);
}


/* sockopt callback — solicitado pelo curl antes de conectar.
   Aumenta o buffer de recepção do socket para SOCK_RCVBUF, o que permite
   que o kernel absorva rajadas de dados da NIC sem bloquear o remetente
   enquanto o curl ainda está processando o chunk anterior.
   O retorno é sempre CURL_SOCKOPT_OK pois a falha no setsockopt é
   não-fatal: o download continua, só que sem o buffer ampliado.       */
static int
sockopt_cb(void *clientp, curl_socket_t curlfd, curlsocktype purpose)
{
    (void)clientp; (void)purpose;
    int rcvbuf = SOCK_RCVBUF;
    setsockopt((int)curlfd, SOL_SOCKET, SO_RCVBUF,
               &rcvbuf, sizeof(rcvbuf));
    return CURL_SOCKOPT_OK;
}


static void udp_send(dl_ctx_t *ctx,
                     int pct, double speed,
                     int64_t downloaded, int64_t total,
                     const char *status)
{
    if (ctx->udp_sock < 0) return;

    char buf[UDP_BUF_SIZE];
    int len = snprintf(buf, sizeof(buf),
        "{\"p\":%d,\"s\":%.0f,\"d\":%lld,\"t\":%lld,\"st\":\"%s\"}",
        pct, speed,
        (long long)downloaded,
        (long long)total,
        status ? status : "");

    sendto(ctx->udp_sock, buf, (size_t)len, 0,
           (struct sockaddr *)&ctx->udp_addr, sizeof(ctx->udp_addr));
}


static int curl_progress_cb(void *clientp, int64_t dltotal, int64_t dlnow, int64_t ultotal, int64_t ulnow)
{
    (void)ultotal; (void)ulnow;

    dl_ctx_t *ctx = (dl_ctx_t *)clientp;
    if (ctx->cancelled)
        return CURLE_ABORTED_BY_CALLBACK;

    time_t now = time(NULL);
    if (now == ctx->last_udp_time)
        return 0;
    ctx->last_udp_time = now;

    double speed = 0.0;
    curl_easy_getinfo(ctx->curl, CURLINFO_SPEED_DOWNLOAD, &speed);

    int64_t total = (dltotal > 0)
        ? (int64_t)(dltotal + ctx->resume_offset)
        : 0;
    int64_t downloaded = (int64_t)(dlnow + ctx->resume_offset);
    int pct = (total > 0) ? (int)((double)downloaded / (double)total * 100.0) : 0;

    udp_send(ctx, pct, speed, downloaded, total, "downloading");

    int notif_step = 25;
    int current_step = (pct / notif_step) * notif_step;

    if (current_step > ctx->last_notif_pct && current_step > 0 && current_step < 100) {
        ctx->last_notif_pct = current_step;
        ps5_notify("exFAT/FFPKG: Baixando %d%%...", current_step);
    }

    return 0;
}

int main(int argc, char **argv)
{
    const char *url   = patched_value(g_patch_url,  sizeof(PATCH_URL_MARKER)  - 1);
    const char *dest  = patched_value(g_patch_dest, sizeof(PATCH_DEST_MARKER) - 1);
    const char *pstr  = patched_value(g_patch_port, sizeof(PATCH_PORT_MARKER) - 1);
    int  port         = pstr ? atoi(pstr) : TARGET_PORT;

    if (!url && argc > 1 && argv[1] && argv[1][0]) url = argv[1];
    if (!dest && argc > 2 && argv[2] && argv[2][0]) dest = argv[2];
    if ((port <= 0 || port > 65535) && argc > 3 && argv[3] && argv[3][0])
        port = atoi(argv[3]);
    if (port <= 0 || port > 65535) port = TARGET_PORT;

    if (!url || !url[0] || !dest || !dest[0]) {
        ps5_notify("exFAT/FFPKG\nURL ou destino ausente no payload.");
        sleep(2);
        return 1;
    }

    const char *fname = file_basename(dest);

    dl_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.udp_sock       = -1;
    ctx.cancelled      = 0;
    ctx.last_udp_time  = 0;
    ctx.last_notif_pct = 0;

    ctx.udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctx.udp_sock >= 0) {
        memset(&ctx.udp_addr, 0, sizeof(ctx.udp_addr));
        ctx.udp_addr.sin_family      = AF_INET;
        ctx.udp_addr.sin_port        = htons((uint16_t)port);
        ctx.udp_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    }

    udp_send(&ctx, 0, 0.0, 0, 0, "starting");
    ps5_notify("Download Iniciado\n%s", fname);

    ensure_dir(dest);

    char resume_path[MAX_PATH + 8];
    snprintf(resume_path, sizeof(resume_path), "%s.resume", dest);

    curl_off_t resume_offset = 0;
    FILE *fp = NULL;

    long existing = get_file_size(resume_path);
    if (existing > 1024) {
        fp = fopen(resume_path, "r+b");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            resume_offset = (curl_off_t)ftell(fp);
            ctx.resume_offset = resume_offset;

            char existing_str[32];
            fmt_size(existing_str, sizeof(existing_str), (int64_t)existing);
            ps5_notify("Retomando Download\n%s a partir de %s", fname, existing_str);
        }
    }

    if (!fp) {
        fp = fopen(resume_path, "w+b");
        resume_offset = 0;
    }

    if (!fp) {
        udp_send(&ctx, 0, 0.0, 0, 0, "error:open_file");
        ps5_notify("Erro de Escrita\nNao foi possivel criar o arquivo em:\n%s", dest);
        if (ctx.udp_sock >= 0) close(ctx.udp_sock);
        sleep(2);
        return 1;
    }

    /* Aumenta o buffer de escrita do libc para FILE_WRITE_BUF.
       Sem isso, cada chunk do curl dispara um write() de 4–8 KB, gerando
       dezenas de milhares de syscalls por GB. Com 2 MB de buffer, o I/O
       fica muito mais batched e o disco (ou exFAT) agradece.           */
    static char s_file_wbuf[FILE_WRITE_BUF];
    setvbuf(fp, s_file_wbuf, _IOFBF, FILE_WRITE_BUF);


    curl_global_init(CURL_GLOBAL_ALL);
    ctx.curl = curl_easy_init();

    if (!ctx.curl) {
        fclose(fp);
        udp_send(&ctx, 0, 0.0, 0, 0, "error:curl_init");
        ps5_notify("Erro Critico\nFalha ao inicializar a biblioteca CURL.");
        if (ctx.udp_sock >= 0) close(ctx.udp_sock);
        curl_global_cleanup();
        sleep(2);
        return 1;
    }

    curl_easy_setopt(ctx.curl, CURLOPT_URL, url);
    curl_easy_setopt(ctx.curl, CURLOPT_USERAGENT, "Mozilla/5.0 (PlayStation; PlayStation 5/10.00)");

    {
        char dns_server[64] = {0};
        if (!get_system_dns(dns_server, sizeof(dns_server))) {
            strncpy(dns_server, "8.8.8.8", sizeof(dns_server) - 1);
        }
        curl_easy_setopt(ctx.curl, CURLOPT_DNS_SERVERS, dns_server);
    }
    
    curl_easy_setopt(ctx.curl, CURLOPT_IPRESOLVE,        CURL_IPRESOLVE_V4);
    curl_easy_setopt(ctx.curl, CURLOPT_DNS_CACHE_TIMEOUT, 60L);

    curl_easy_setopt(ctx.curl, CURLOPT_BUFFERSIZE,      1048576L);  /* 1 MiB buffer interno do curl */
    curl_easy_setopt(ctx.curl, CURLOPT_TCP_KEEPALIVE,   1L);
    curl_easy_setopt(ctx.curl, CURLOPT_TCP_KEEPIDLE,    30L);       /* inicia keepalive após 30 s idle */
    curl_easy_setopt(ctx.curl, CURLOPT_TCP_KEEPINTVL,   10L);       /* retransmite probe a cada 10 s   */
    curl_easy_setopt(ctx.curl, CURLOPT_TCP_NODELAY,     1L);        /* desativa Nagle; reduz latência ACK */
    curl_easy_setopt(ctx.curl, CURLOPT_SOCKOPTFUNCTION, sockopt_cb);/* amplia buffer de recepção do socket */
    /* Tenta HTTP/2 sobre TLS; se o servidor não suportar, cai para 1.1
       automaticamente — sem risco de quebrar downloads existentes.    */
    curl_easy_setopt(ctx.curl, CURLOPT_HTTP_VERSION,    CURL_HTTP_VERSION_2TLS);

    curl_easy_setopt(ctx.curl, CURLOPT_SSL_VERIFYHOST,   0L);
    curl_easy_setopt(ctx.curl, CURLOPT_SSL_VERIFYPEER,   0L);
    curl_easy_setopt(ctx.curl, CURLOPT_SSLVERSION,       CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(ctx.curl, CURLOPT_USE_SSL,          CURLUSESSL_TRY);
    curl_easy_setopt(ctx.curl, CURLOPT_FOLLOWLOCATION,   1L);
    curl_easy_setopt(ctx.curl, CURLOPT_CONNECTTIMEOUT,   20L);
    curl_easy_setopt(ctx.curl, CURLOPT_FAILONERROR,      1L);
    curl_easy_setopt(ctx.curl, CURLOPT_WRITEFUNCTION,    fwrite);
    curl_easy_setopt(ctx.curl, CURLOPT_WRITEDATA,        fp);
    curl_easy_setopt(ctx.curl, CURLOPT_NOPROGRESS,       0L);
    curl_easy_setopt(ctx.curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(ctx.curl, CURLOPT_XFERINFODATA,     &ctx);

    if (resume_offset > 0)
        curl_easy_setopt(ctx.curl, CURLOPT_RESUME_FROM_LARGE, resume_offset);

    CURLcode result = curl_easy_perform(ctx.curl);

    curl_easy_cleanup(ctx.curl);
    ctx.curl = NULL;
    fclose(fp);
    curl_global_cleanup();

    int exit_ok = 0;
    if (result == CURLE_OK) {
        if (rename(resume_path, dest) != 0) {
            udp_send(&ctx, 0, 0.0, 0, 0, "error:rename");
            ps5_notify("Erro ao Finalizar\n%s", strerror(errno));
        } else {
            int64_t final_size = get_file_size(dest);
            udp_send(&ctx, 100, 0.0, final_size, final_size, "completed");
            ps5_notify("Download Concluido!\n%s", fname);
            exit_ok = 1;
        }
    } else if (result == CURLE_ABORTED_BY_CALLBACK) {
        udp_send(&ctx, 0, 0.0, 0, 0, "cancelled");
        ps5_notify("Download Cancelado\n%s", fname);
    } else {
        udp_send(&ctx, 0, 0.0, 0, 0, "error:download");
        ps5_notify("Erro no Download\nCodigo CURL: %d", (int)result);
    }

    if (ctx.udp_sock >= 0)
        close(ctx.udp_sock);

    sleep(2);

    return exit_ok ? 0 : 1;
}
