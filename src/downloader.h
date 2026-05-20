/* Sonic Loader - monitored exFAT / FFPKG downloader bridge. */

#pragma once

#include <microhttpd.h>

enum MHD_Result downloader_request(struct MHD_Connection *conn,
                                   const char *url);
