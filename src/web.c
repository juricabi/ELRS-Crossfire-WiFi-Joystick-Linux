/* PLACEHOLDER until the web server is written (see web.h, docs/WEB_API.md): lets the program link. */
#include "web.h"

#include <stdio.h>

web_t *web_start(engine_t *e, const char *addr, uint16_t port, char *err, size_t cap) {
  (void)e; (void)addr; (void)port;
  snprintf(err, cap, "the web page is not built into this version");
  return NULL;
}

void web_url(const web_t *w, char *out, size_t cap) {
  (void)w;
  if (cap) out[0] = 0;
}

void web_stop(web_t *w) { (void)w; }
