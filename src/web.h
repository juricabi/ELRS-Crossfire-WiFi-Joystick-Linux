/* The web page and its JSON API (docs/WEB_API.md), served on its own thread. It reads the engine
 * through eng_snapshot / eng_log_read and changes it through eng_update_config / eng_start / eng_stop /
 * eng_restart_stream only: it never touches the receive path. */
#ifndef WEB_H
#define WEB_H

#include <stddef.h>
#include <stdint.h>
#include "engine.h"

#define WEB_ADDR_DEFAULT "127.0.0.1"
#define WEB_PORT_DEFAULT 11000          /* TCP; the joystick stream is UDP 11000, a different port space */

typedef struct web web_t;

/* Bind addr:port (an IPv4 literal: "127.0.0.1" = this machine only, "0.0.0.0" = every network) and
 * serve on a new thread. NULL with err (e.g. "cannot listen on 127.0.0.1:11000: address already in
 * use") when it can't. */
web_t *web_start(engine_t *e, const char *addr, uint16_t port, char *err, size_t cap);
/* "http://127.0.0.1:11000/" (for 0.0.0.0: "http://<this machine's IP>:11000/" when one is known) */
void   web_url(const web_t *w, char *out, size_t cap);
void   web_stop(web_t *w);             /* close every connection, end the thread, free; NULL is fine */

#endif
