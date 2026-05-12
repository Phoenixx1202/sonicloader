/* Sonic Loader — etaHEN-compatible jailbreak IPC daemon.

   Apps that ship the universalps5 PRX (jb.zip) call FreeUnjail() at
   launch. That tries two paths: a TCP/IPC connection to
   127.0.0.1:9028 with a HijackerCommand{magic=0xDEADBEEF, cmd=
   JAILBREAK_CMD, PID=getpid()} struct, or a "networkless" file drop
   at /download0/etahen_jailbreak containing JSON {"PID":"<pid>"}.

   This daemon implements both paths so apps that depend on etaHEN's
   IPC protocol just work when launched while Sonic Loader is running.
   Each command emits a debug notification so the user can see exactly
   which function fired and which PID was escalated. */

#pragma once

#include <sys/types.h>

void jb_start(void);

/* Apply the same ucred / rootdir / authid / caps / attrs escalation
   to the given PID. Used both by the IPC server and by main() to
   jailbreak Sonic-Loader's own process at boot so every file op the
   File Manager fires (open/read/write/mkdir/unlink/rename) runs with
   full kernel privilege regardless of source or destination mount. */
int  jb_escalate_pid(pid_t pid);
