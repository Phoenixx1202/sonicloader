/* Sonic Loader — etaHEN-compatible jailbreak IPC daemon.

   Two listener threads:

   1. cmd_thread — TCP listener on 127.0.0.1:9028 that speaks the
      HijackerCommand protocol the universalps5 PRX uses (jb.zip). The
      protocol is small: a fixed-size struct of {magic, cmd, PID, ret,
      msg1, msg2}. We serve the JAILBREAK_CMD specifically because
      that's what apps need to escape their sandbox; the other commands
      (LAUNCH/PROCLIST/KILL/KILL_APP/ACTIVE) get an OK reply with a
      notification so the calling app moves on without erroring.

   2. file_thread — polls /download0/etahen_jailbreak every 250 ms for
      the "networkless" path. The PRX writes that file with
      {"PID":"<pid>"} when it can't open the TCP socket; we detect new
      PIDs, jailbreak them, and remove the file so the PRX's polling
      loop exits.

   The actual jailbreak is the same write pattern etaHEN's
   Hijacker::jailbreak() uses: zero out cr_uid/cr_ruid/cr_svuid/
   cr_rgid/cr_ngroups in the target's ucred, swap fd_rdir/fd_jdir for
   the kernel's rootvnode (sandbox escape), then set cr_sceAuthID=
   0x4801000000000013 and cr_sceCaps[0..1]=-1. The ps5-payload-sdk
   exposes helpers for every one of these so we don't poke kernel
   memory by hand. */

#define _BSD_SOURCE         /* expose u_short/u_int for sys/event.h */
#define __BSD_VISIBLE 1

#include <sys/types.h>      /* must precede sys/event.h on this SDK */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>      /* kqueue / EVFILT_VNODE */
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "jb.h"
#include "ps5/notify.h"


#define JB_PORT          9028
#define JB_FILE          "/download0/etahen_jailbreak"
#define JB_MAGIC         0xDEADBEEF
#define JB_AUTHID        0x4801000000000013ULL


/* Mirror of HijackerCommand in jb.zip's prx.cpp — same wire layout, no
   alignment surprises (everything is 4-byte aligned, magic is the
   first field). 0x10 + 0x500 + 0x500 = 0x1010. */
struct hijacker_cmd {
  uint32_t magic;
  int8_t   cmd;
  /* Compiler will pad to 4-byte boundary here; the upstream prx.cpp
     leaves the same gap. */
  int8_t   _pad[3];
  int32_t  pid;
  int32_t  ret;
  char     msg1[0x500];
  char     msg2[0x500];
} __attribute__((packed));


enum jb_command {
  JB_CMD_INVALID   = -1,
  JB_CMD_ACTIVE    = 0,
  JB_CMD_LAUNCH    = 1,
  JB_CMD_PROCLIST  = 2,
  JB_CMD_KILL      = 3,
  JB_CMD_KILL_APP  = 4,
  JB_CMD_JAILBREAK = 5,
};


static const char*
jb_cmd_name(int8_t cmd) {
  switch(cmd) {
    case JB_CMD_ACTIVE:    return "ACTIVE";
    case JB_CMD_LAUNCH:    return "LAUNCH";
    case JB_CMD_PROCLIST:  return "PROCLIST";
    case JB_CMD_KILL:      return "KILL";
    case JB_CMD_KILL_APP:  return "KILL_APP";
    case JB_CMD_JAILBREAK: return "JAILBREAK";
    default:               return "INVALID";
  }
}


/* Replicates Hijacker::jailbreak() from etaHEN/libhijacker. The
   ps5-payload-sdk wraps every kernel write we need:
     kernel_set_ucred_uid / _ruid / _svuid / _rgid / _svgid → cr_*
     kernel_set_ucred_authid                               → cr_sceAuthID
     kernel_set_ucred_caps                                 → cr_sceCaps[]
     kernel_set_ucred_attrs                                → cr_sceAttr[]
     kernel_set_proc_rootdir / _jaildir + kernel_get_root_vnode
                                                            → escape sandbox
   Returns 0 on full success, -1 on any failure. */
int
jb_escalate_pid(pid_t pid) {
  if(pid <= 0) return -1;

  /* Make sure the proc still exists. */
  intptr_t proc = kernel_get_proc(pid);
  if(!proc) return -1;

  int rc = 0;

  /* uid → 0 (root) on every cred slot the kernel checks. */
  if(kernel_set_ucred_uid (pid, 0) != 0) rc = -1;
  if(kernel_set_ucred_ruid(pid, 0) != 0) rc = -1;
  if(kernel_set_ucred_svuid(pid, 0)!= 0) rc = -1;
  if(kernel_set_ucred_rgid(pid, 0) != 0) rc = -1;
  if(kernel_set_ucred_svgid(pid,0) != 0) rc = -1;

  /* Sandbox escape: point the proc's rootdir + jaildir at the kernel
     root vnode so the app sees the real "/" instead of its sandboxed
     /system_data/priv/appmeta/<id>/... view. */
  intptr_t rootvnode = kernel_get_root_vnode();
  if(rootvnode) {
    if(kernel_set_proc_rootdir(pid, rootvnode) != 0) rc = -1;
    if(kernel_set_proc_jaildir(pid, rootvnode) != 0) rc = -1;
  }

  /* Sony privilege bump — the caps + authid combo etaHEN uses. */
  if(kernel_set_ucred_authid(pid, JB_AUTHID) != 0) rc = -1;
  uint8_t caps[16];
  memset(caps, 0xff, sizeof(caps));
  if(kernel_set_ucred_caps(pid, caps) != 0) rc = -1;

  /* cr_sceAttr[0] = 0x80 — single byte high-attr flag. The SDK helper
     takes a uint64_t we write at the attr offset; 0x80 is the value
     etaHEN writes. */
  if(kernel_set_ucred_attrs(pid, 0x80) != 0) rc = -1;

  return rc;
}


/* Per-pid de-dup so a noisy app spamming retries doesn't fire a
   notification every loop. We track the last 16 PIDs handled. */
#define JB_LRU_SIZE 16
static pid_t g_recent[JB_LRU_SIZE];
static int   g_recent_idx = 0;
static pthread_mutex_t g_recent_lock = PTHREAD_MUTEX_INITIALIZER;

static int
jb_recently_seen(pid_t pid) {
  int seen = 0;
  pthread_mutex_lock(&g_recent_lock);
  for(int i = 0; i < JB_LRU_SIZE; i++) {
    if(g_recent[i] == pid) { seen = 1; break; }
  }
  if(!seen) {
    g_recent[g_recent_idx] = pid;
    g_recent_idx = (g_recent_idx + 1) % JB_LRU_SIZE;
  }
  pthread_mutex_unlock(&g_recent_lock);
  return seen;
}


static void
jb_handle_jailbreak(pid_t pid, const char *source) {
  int rc = jb_escalate_pid(pid);
  /* Per-PID rate-limit the user-facing toast; the system log line
     always fires. */
  int recent = jb_recently_seen(pid);
  if(rc == 0) {
    printf("jb: jailbroke pid=%d via %s\n", (int)pid, source);
    if(!recent) {
      notify("Sonic-Loader JB: pid %d escalated via %s", (int)pid, source);
    }
  } else {
    printf("jb: FAILED for pid=%d via %s\n", (int)pid, source);
    notify("Sonic-Loader JB: pid %d FAILED via %s", (int)pid, source);
  }
}


/* ───── TCP server (port 9028) ───── */

static void
jb_handle_client(int sock) {
  struct hijacker_cmd cmd;
  memset(&cmd, 0, sizeof(cmd));

  ssize_t n = recv(sock, &cmd, sizeof(cmd), 0);
  if(n < (ssize_t)sizeof(cmd)) {
    /* Clients sometimes send less than the full struct on first
       byte — try once more. */
    while(n > 0 && n < (ssize_t)sizeof(cmd)) {
      ssize_t r = recv(sock, ((uint8_t*)&cmd) + n, sizeof(cmd) - n, 0);
      if(r <= 0) break;
      n += r;
    }
    if(n < (ssize_t)sizeof(cmd)) {
      cmd.ret = -1;
      send(sock, &cmd, sizeof(cmd), MSG_NOSIGNAL);
      return;
    }
  }

  notify("JB IPC ← %s (pid %d)", jb_cmd_name(cmd.cmd), (int)cmd.pid);

  if(cmd.magic != JB_MAGIC) {
    cmd.ret = -1;
    send(sock, &cmd, sizeof(cmd), MSG_NOSIGNAL);
    notify("JB IPC: bad magic 0x%08x", cmd.magic);
    return;
  }

  switch(cmd.cmd) {
    case JB_CMD_JAILBREAK:
      if(cmd.pid <= 0 || !kernel_get_proc(cmd.pid)) {
        cmd.ret = -1;
        notify("JB IPC: bad pid %d", (int)cmd.pid);
      } else {
        jb_handle_jailbreak(cmd.pid, "ipc:9028");
        cmd.ret = 0;
      }
      break;

    case JB_CMD_ACTIVE:
      /* etaHEN's "are you alive" probe — reply OK so the PRX moves on. */
      cmd.ret = 0;
      break;

    case JB_CMD_LAUNCH:
    case JB_CMD_PROCLIST:
    case JB_CMD_KILL:
    case JB_CMD_KILL_APP:
      /* Soft-acknowledge the rest. We don't implement etaHEN's full
         remote-launch surface (we have our own /launch endpoint), but
         apps that ping these don't expect failure. */
      cmd.ret = 0;
      break;

    default:
      cmd.ret = -1;
      break;
  }

  send(sock, &cmd, sizeof(cmd), MSG_NOSIGNAL);
}


static void*
jb_cmd_thread(void *unused) {
  (void)unused;

  int s = socket(AF_INET, SOCK_STREAM, 0);
  if(s < 0) {
    notify("JB IPC: socket failed (%s)", strerror(errno));
    return NULL;
  }
  int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family      = AF_INET;
  addr.sin_port        = htons(JB_PORT);
  /* Bind to all interfaces — same as etaHEN — so the PRX's connect to
     127.0.0.1:9028 works regardless of which loopback view it sees. */
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if(bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    notify("JB IPC: bind :%d failed (%s)", JB_PORT, strerror(errno));
    close(s);
    return NULL;
  }
  if(listen(s, 8) < 0) {
    notify("JB IPC: listen failed (%s)", strerror(errno));
    close(s);
    return NULL;
  }

  printf("jb: IPC server listening on 0.0.0.0:%d\n", JB_PORT);

  for(;;) {
    int c = accept(s, NULL, NULL);
    if(c < 0) {
      if(errno == EINTR) continue;
      break;
    }
    jb_handle_client(c);
    close(c);
  }
  close(s);
  return NULL;
}


/* ───── /download0/etahen_jailbreak watcher ───── */

static int
parse_pid_from_json(const char *txt, size_t len) {
  /* The PRX writes {"PID":"<digits>"}. We don't pull in cJSON for
     three lines of state so do a minimal scan. */
  const char *p = strstr(txt, "\"PID\"");
  if(!p) return -1;
  p += 5;
  while(p < txt + len && (*p == ' ' || *p == ':' || *p == '"')) p++;
  if(p >= txt + len) return -1;
  pid_t pid = 0;
  while(p < txt + len && *p >= '0' && *p <= '9') {
    pid = pid * 10 + (*p - '0');
    p++;
  }
  return pid > 0 ? pid : -1;
}


/* Try to handle the marker file once. Idempotent: if the file is
   absent / empty / mid-write, returns 0. If we successfully escalated
   a PID, returns 1. Always unlinks the file on exit so the PRX's
   inner wait loop notices the disappearance. */
static int
jb_handle_marker_file(void) {
  struct stat st;
  if(stat(JB_FILE, &st) != 0)            return 0;
  if(st.st_size <= 0 || st.st_size >= 4096) {
    unlink(JB_FILE);
    return 0;
  }

  int fd = open(JB_FILE, O_RDONLY);
  if(fd < 0) return 0;
  char buf[4096];
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);

  int handled = 0;
  if(n > 0) {
    buf[n] = 0;
    pid_t pid = parse_pid_from_json(buf, (size_t)n);
    if(pid > 0 && kernel_get_proc(pid)) {
      jb_handle_jailbreak(pid, "file:/download0");
      handled = 1;
    }
  }
  unlink(JB_FILE);
  return handled;
}


static void*
jb_file_thread(void *unused) {
  (void)unused;

  /* Make sure the parent dir exists so the PRX's open() succeeds even
     when nothing else has touched /download0 yet. */
  mkdir("/download0", 0777);

  /* Process anything already on disk before we install the watcher. */
  jb_handle_marker_file();

  /* Use kqueue with EVFILT_VNODE on /download0 so we get woken the
     instant a file is created/written there, instead of paying poll
     latency. PS5 inherits FreeBSD's kqueue. We keep a tiny 2 ms
     backstop poll in case any kernel build skips an EVFILT_VNODE
     event (the cost is trivial — a single stat()). */
  int kq = kqueue();
  int dirfd = open("/download0", O_RDONLY | O_DIRECTORY);

  if(kq >= 0 && dirfd >= 0) {
    struct kevent ev;
    EV_SET(&ev, dirfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
           NOTE_WRITE | NOTE_LINK | NOTE_EXTEND | NOTE_ATTRIB,
           0, NULL);
    if(kevent(kq, &ev, 1, NULL, 0, NULL) == 0) {
      printf("jb: file watcher armed on /download0 via kqueue\n");
      for(;;) {
        struct kevent out;
        struct timespec to = { .tv_sec = 0, .tv_nsec = 2 * 1000 * 1000 };
        /* Wait at most 2 ms; that bounds how late we can be relative
           to the PRX's own 0.5 ms inner sleep. */
        int n = kevent(kq, NULL, 0, &out, 1, &to);
        if(n < 0 && errno != EINTR) break;
        /* Whether we got an event or hit the timeout, re-check the
           file. The handler is cheap when nothing is there. */
        jb_handle_marker_file();
      }
      close(dirfd);
      close(kq);
    }
  }

  /* Fall-through fallback if kqueue setup failed. Unlikely on PS5 but
     not worth crashing over. */
  if(dirfd >= 0) close(dirfd);
  if(kq    >= 0) close(kq);
  printf("jb: file watcher fell back to fast poll (kqueue unavailable)\n");
  for(;;) {
    jb_handle_marker_file();
    usleep(2 * 1000);  /* 2 ms */
  }
  return NULL;
}


void
jb_start(void) {
  pthread_t t;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);

  pthread_create(&t, &a, jb_cmd_thread,  NULL);
  pthread_create(&t, &a, jb_file_thread, NULL);

  pthread_attr_destroy(&a);
}
