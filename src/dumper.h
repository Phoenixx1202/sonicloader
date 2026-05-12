/* Sonic Loader — EchoStretch/ps5-app-dumper bridge.

   GET /api/dumper/run    spawn ps5-app-dumper.elf detached. */

#pragma once

#include <microhttpd.h>

enum MHD_Result dumper_request(struct MHD_Connection *conn, const char *url);
