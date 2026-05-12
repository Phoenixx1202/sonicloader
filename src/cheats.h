/* Sonic Loader — cheat engine.

   Cheat files live in /data/sonic-loader/cheats/<TITLE_ID>.json and use
   the Sonic-Loader CheatManager schema:

   {
     "title_id": "PPSA12345",
     "name":     "Game Name",
     "cheats":   [
       {
         "name": "Infinite Health",
         "mods": [
           {
             "offset":   "0x12340",      // hex (with or without 0x)
             "on":       "9090909090",   // hex byte stream to write
             "off":      "0102030405",   // hex byte stream to restore
             "absolute": false           // if true, offset is absolute
                                         // otherwise it is relative to
                                         // the eboot image base
           }
         ]
       }
     ]
   }

   All HTTP routes return JSON. */

#pragma once

#include <microhttpd.h>

enum MHD_Result cheats_request(struct MHD_Connection *conn, const char *url,
                               const char *method, const char *upload_data,
                               size_t *upload_data_size, void **con_cls);

/* Make sure /data/sonic-loader/cheats exists so the user can FTP files
   straight into it. Called once at startup. */
void cheats_init(void);

/* Master enable flag — when off, no cheats are applied/reverted. */
int  cheats_engine_enabled(void);
void cheats_engine_set_enabled(int on);
