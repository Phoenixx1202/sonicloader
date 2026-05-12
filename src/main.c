/* Sonic Loader — all-in-one PS5 payload.
   Bundles kstuff + ftpsrv + ShadowMountPlus and serves a web UI for
   launching any title in app.db. */

#include <stdio.h>
#include <signal.h>
#include <unistd.h>

#include "activity.h"
#include "cheats.h"
#include "config.h"
#include "releases.h"
#include "fan.h"
#include "jb.h"
#include "kmonitor.h"
#include "mdns.h"
#include "homebrew.h"
#include "smp_meta.h"
#include "sys.h"
#include "websrv.h"
#include "y2jb_updater.h"


int
main(int argc, char** argv) {
  const uint16_t port = 6969;

  puts(".----------------------------------------------------------.");
  puts("|   ____              _        _                   _       |");
  puts("|  / ___|  ___  _ __ (_) ___  | |    ___   __ _  __| | ___ |");
  puts("|  \\___ \\ / _ \\| '_ \\| |/ __| | |   / _ \\ / _` |/ _` |/ _ \\|");
  puts("|   ___) | (_) | | | | | (__  | |__| (_) | (_| | (_| |  __/|");
  puts("|  |____/ \\___/|_| |_|_|\\___| |_____\\___/ \\__,_|\\__,_|\\___||");
  puts("|                                                          |");
  printf("|  %-22s    all-in-one PS5 payload     |\n", VERSION_TAG);
  puts("'----------------------------------------------------------'");
  puts("");
  puts("  bundled: kstuff-lite + klogsrv + ftpsrv + ShadowMountPlus + nanoDNS +");
  puts("           BackPork + garlic-worker + garlic-savemgr + np-fake-signin +");
  puts("           np-restore-account + websrv + kmon");

  signal(SIGPIPE, SIG_IGN);
  signal(SIGCHLD, SIG_IGN);

#ifdef __SCE__
  /* Self-jailbreak before doing anything else. Sonic Loader is the
     ELF launched by elfldr; without an explicit ucred + rootdir/
     jaildir escalation the process inherits a sandboxed view of the
     filesystem and most file ops on /mnt/ext1, /system_data, etc.
     fail silently with EACCES inside the File Manager / FTP / cheat
     engine paths. After this call, every userspace open()/read()/
     write()/mkdir()/unlink()/rename() runs with uid=0, the kernel
     rootvnode for fd_rdir + fd_jdir, all sceCaps set, and the
     ShellCore-class authid — so copy/move/delete works in every
     direction (USB ↔ /data ↔ /mnt/ext1 ↔ /system_data ↔ anywhere
     kstuff has unlocked). */
  jb_escalate_pid(getpid());

  /* Create /data/sonic-loader/cheats early so the bundled ftpsrv can drop
     uploaded cheat .json files straight into it. */
  cheats_init();
  /* Load persisted per-title activity stats from
     /data/sonic-loader/activity.json so the spotlight overlay has
     numbers from the moment the launcher first paints. */
  activity_init();
  /* Background prefetch the GitHub release lists for the kstuff +
     SMP pickers. First boot fills the disk cache so subsequent
     opens are instant; later boots see the cache and no-op. */
  releases_init();

  sys_spawn_embedded_payloads();
  /* klogsrv was started inside sys_spawn_embedded_payloads(); give it a
     beat to bind its TCP socket before kmonitor tries to connect. */
  sleep(2);
  kmonitor_start();

  /* Fan watcher re-applies the user's pinned threshold on a 15 s tick
     so the firmware-side fan reset on app/game launch can't undo it. */
  fan_init();

  /* Garlic Worker is community infrastructure for garlicsaves.com — it
     processes save-encrypt/decrypt jobs while the console is idle, with
     ~zero impact on active gameplay. We start it by default; the user
     can disable it from Settings, and config_load() below will pick up
     a previous "off" preference if one was persisted. */
  sys_garlic_seed_config();
  sys_garlic_worker_set_enabled(1);

  /* etaHEN-compatible jailbreak IPC. Apps that ship the universalps5
     PRX (jb.zip) connect to 127.0.0.1:9028 (or drop a JSON file at
     /download0/etahen_jailbreak) at launch and expect a daemon to
     escalate their PID. Implementing this server here makes those
     apps work directly on Sonic Loader without etaHEN running. */
  jb_start();

#ifndef SONIC_NO_ETAHEN
  /* Spawn the bundled etaHEN daemon on by default in the etaHEN
     variant. Both build variants (etaHEN + no-etaHEN) can now JB
     apps like Itemzflow and xplorer through the IPC daemon above —
     but with etaHEN's toolbox running, those apps escalate
     noticeably faster because the toolbox keeps PID-elevation
     primitives hot. config_load() below restores a persisted
     "off" preference if the user had explicitly disabled it. */
  sys_etahen_set_enabled(1);
#endif

  /* Replay /data/sonic-loader/config.ini through every subsystem so
     last-session settings (kstuff auto-toggle, cheat engine on/off,
     pause/resume delays, fan threshold, BackPork, Garlic) come back
     without the user having to re-toggle them. config_save() is wired
     into every setter so any subsequent change is persisted
     automatically. */
  config_load();

  /* Heal /user/appmeta/<TITLE_ID>/icon0.png + param.json gaps that
     SMP sometimes leaves when scan-mounting USB / extended-storage
     games. The watcher runs every 30 s by default and copies missing
     metadata from each game's sce_sys/. SMP itself stays unmodified
     (it's a per-release GitHub download we can't sanely patch). */
  smp_meta_init();

  /* The wake-watcher / smp-bootkick daemon was removed entirely in
     1.0.50 — even the trimmed one-shot variant from 1.0.49 was
     hitting the console hard enough to be unreliable in practice.
     After waking from rest mode, users resend the loader (or open
     it via the home-screen tile / Homebrew Loader / a PC browser)
     and tap Settings → ShadowMountPlus → Restart SMP manually. */

  /* Background silent-verify of any ps5_autoloader/ folders that host
     a sonic-loader build. Sleeps ~30 s before its first sweep so the
     network stack, DNS, and the boot-time release prefetch have all
     settled — running the verify any earlier ends up racing those and
     the http_get fails. After the first sweep the thread exits; the
     foreground "Update all" button in Settings → Y2JB autoloader sync
     is still available for on-demand refreshes. */
  y2jb_startup_init();

  /* Auto-install the Sonic Loader home-screen tile PKG every boot.
     Background thread: sleeps 30 s for the network to settle, fetches
     payloads/sonic-loader-tile.pkg from the project repo, installs
     it via the same code path users used to trigger from Settings.
     Idempotent — reinstalling the same contentId is a no-op-with-
     overwrite, so a tile that's already present just keeps the
     latest version. Failures stderr-log only, no UI noise. */
  homebrew_auto_install_tile_init();
#endif

  /* :8080 compat listener removed in 1.0.55 — Sonic Loader now
     serves only :6969. The upstream PS5 launcher PKG (which used
     to hardcode :8080) is no longer being relied on as a tile;
     the new home-screen-tile path will deeplink to :6969 directly
     via app.db / param.json synthesis. Users who still want the
     legacy upstream tile can install Sonic Loader 1.0.45–1.0.54
     to keep the dual-port behavior. */

  while(1) {
#ifdef __SCE__
    mdns_discovery_start();
#endif
    websrv_listen(port);
    sleep(3);
  }

  return 0;
}
