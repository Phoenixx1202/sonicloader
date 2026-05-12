/* Sonic Loader — persistent settings, /data/sonic-loader/config.ini. */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "config.h"
#include "kmonitor.h"
#include "cheats.h"
#include "fan.h"
#include "sys.h"

#define CONFIG_DIR  "/data/sonic-loader"
#define CONFIG_PATH "/data/sonic-loader/config.ini"


static int
parse_bool(const char *v, int dflt) {
  if(!v) return dflt;
  while(*v == ' ' || *v == '\t') v++;
  if(*v == '1' || !strcasecmp(v, "true") || !strcasecmp(v, "on") ||
     !strcasecmp(v, "yes")) return 1;
  if(*v == '0' || !strcasecmp(v, "false") || !strcasecmp(v, "off") ||
     !strcasecmp(v, "no"))  return 0;
  return dflt;
}


static int
parse_int(const char *v, int dflt) {
  if(!v) return dflt;
  while(*v == ' ' || *v == '\t') v++;
  if(!*v) return dflt;
  return (int)strtol(v, NULL, 10);
}


/* Read the config file if it exists; for each recognised key, push the
   value into the corresponding subsystem setter. */
void
config_load(void) {
  mkdir("/data", 0755);
  mkdir(CONFIG_DIR, 0755);

  FILE *f = fopen(CONFIG_PATH, "r");
  if(!f) {
    /* No config yet — leave subsystems at their compile-time defaults. */
    return;
  }

  char line[256];
  while(fgets(line, sizeof(line), f)) {
    /* Strip CR/LF and leading whitespace. */
    char *p = line;
    while(*p == ' ' || *p == '\t') p++;
    size_t n = strlen(p);
    while(n > 0 && (p[n-1] == '\n' || p[n-1] == '\r' ||
                    p[n-1] == ' '  || p[n-1] == '\t')) p[--n] = 0;
    if(!*p || *p == '#' || *p == ';') continue;

    char *eq = strchr(p, '=');
    if(!eq) continue;
    *eq = 0;
    char *k = p;
    char *v = eq + 1;
    /* Trim trailing whitespace on key. */
    char *kend = k + strlen(k);
    while(kend > k && (kend[-1] == ' ' || kend[-1] == '\t')) *--kend = 0;
    while(*v == ' ' || *v == '\t') v++;

    if(!strcmp(k, "kstuff_auto_toggle")) {
      kmonitor_set_auto_toggle(parse_bool(v, 0));
    } else if(!strcmp(k, "cheats_engine")) {
      cheats_engine_set_enabled(parse_bool(v, 1));
    } else if(!strcmp(k, "backpork")) {
      sys_backpork_set_enabled(parse_bool(v, 0));
    } else if(!strcmp(k, "pause_seconds")) {
      int p_s = 25, r_s = 10;
      kmonitor_get_delays(&p_s, &r_s);
      kmonitor_set_delays(parse_int(v, 25), r_s);
    } else if(!strcmp(k, "resume_seconds")) {
      int p_s = 25, r_s = 10;
      kmonitor_get_delays(&p_s, &r_s);
      kmonitor_set_delays(p_s, parse_int(v, 10));
    } else if(!strcmp(k, "fan_threshold")) {
      int t = parse_int(v, 0);
      if(t >= 30 && t <= 90) fan_pin_threshold(t);
    } else if(!strcmp(k, "garlic_worker")) {
      sys_garlic_worker_set_enabled(parse_bool(v, 1));
    } else if(!strcmp(k, "garlic_savemgr")) {
      sys_garlic_savemgr_set_enabled(parse_bool(v, 0));
    } else if(!strcmp(k, "garlic_poll_interval")) {
      int n = parse_int(v, 30);
      sys_garlic_set_poll_interval(n);
    } else if(!strcmp(k, "etahen")) {
      /* Default-on as of 1.0.53 — both build variants now JB the
         common app set, but the toolbox makes Itemzflow / xplorer
         elevate noticeably faster. Users who don't want it persist
         "etahen=0" via the Settings toggle. */
      sys_etahen_set_enabled(parse_bool(v, 1));
    } else if(!strcmp(k, "ftp_port")) {
      int n = parse_int(v, 2121);
      if(n >= 1 && n <= 65535) sys_ftpsrv_set_port(n);
    } else if(!strcmp(k, "ftp_user")) {
      sys_ftpsrv_set_user(v);
    } else if(!strcmp(k, "ftp_pass")) {
      sys_ftpsrv_set_pass(v);
    } else if(!strcmp(k, "ftp_type")) {
      sys_ftpsrv_set_type(v);
    } else if(!strcmp(k, "smp_defaults")) {
      extern void sys_smp_defaults_set(int);
      sys_smp_defaults_set(parse_bool(v, 1));
    } else if(!strcmp(k, "smp_manual_scanpaths")) {
      extern void sys_smp_manual_paths_load(const char*);
      sys_smp_manual_paths_load(v);
    }
    /* Unknown keys silently ignored — forward compatibility. */
  }
  fclose(f);
  printf("config: loaded %s\n", CONFIG_PATH);
}


void
config_save(void) {
  mkdir("/data", 0755);
  mkdir(CONFIG_DIR, 0755);

  /* Snapshot every value before opening the file to keep the write
     atomic-ish (small file, one fwrite). */
  int auto_toggle = kmonitor_auto_toggle_enabled();
  int cheats_on   = cheats_engine_enabled();
  int backpork_on = sys_backpork_is_running();
  int pause_s = 25, resume_s = 10;
  kmonitor_get_delays(&pause_s, &resume_s);
  int fan_t = fan_pinned_threshold();   /* 0 = unpinned */
  int garlic_worker_on  = sys_garlic_worker_is_running();
  int garlic_savemgr_on = sys_garlic_savemgr_is_running();
  int garlic_poll       = sys_garlic_get_poll_interval();
  int etahen_on         = sys_etahen_is_running();

  char tmp[64];
  snprintf(tmp, sizeof(tmp), CONFIG_PATH ".tmp");

  FILE *f = fopen(tmp, "w");
  if(!f) {
    perror("config_save: fopen");
    return;
  }
  fprintf(f, "# Sonic Loader settings — written automatically by the web UI.\n");
  fprintf(f, "# Manual edits survive a redeploy; format is one key=value per line.\n");
  fprintf(f, "kstuff_auto_toggle=%d\n", auto_toggle ? 1 : 0);
  fprintf(f, "cheats_engine=%d\n",      cheats_on   ? 1 : 0);
  fprintf(f, "backpork=%d\n",           backpork_on ? 1 : 0);
  fprintf(f, "pause_seconds=%d\n",      pause_s);
  fprintf(f, "resume_seconds=%d\n",     resume_s);
  if(fan_t > 0) {
    fprintf(f, "fan_threshold=%d\n",    fan_t);
  }
  fprintf(f, "garlic_worker=%d\n",          garlic_worker_on  ? 1 : 0);
  fprintf(f, "garlic_savemgr=%d\n",         garlic_savemgr_on ? 1 : 0);
  fprintf(f, "garlic_poll_interval=%d\n",   garlic_poll);
  fprintf(f, "etahen=%d\n",                 etahen_on ? 1 : 0);
  fprintf(f, "ftp_port=%d\n",               sys_ftpsrv_get_port());
  fprintf(f, "ftp_user=%s\n",               sys_ftpsrv_get_user());
  fprintf(f, "ftp_pass=%s\n",               sys_ftpsrv_get_pass());
  fprintf(f, "ftp_type=%s\n",               sys_ftpsrv_get_type());
  {
    extern int sys_smp_defaults_get(void);
    extern void sys_smp_manual_paths_serialize(char*, size_t);
    char paths_csv[2048];
    sys_smp_manual_paths_serialize(paths_csv, sizeof(paths_csv));
    fprintf(f, "smp_defaults=%d\n",         sys_smp_defaults_get());
    fprintf(f, "smp_manual_scanpaths=%s\n", paths_csv);
  }
  fclose(f);

  /* Replace atomically. */
  if(rename(tmp, CONFIG_PATH) != 0) {
    perror("config_save: rename");
    unlink(tmp);
  }
}
