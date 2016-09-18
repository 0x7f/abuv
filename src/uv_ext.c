#include "uv_ext.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

uv_buf_t uv_buf_new() { return uv_buf_init(NULL, 0); }

void uv_buf_reset(uv_buf_t *buf) {
  if (buf->base) {
    free(buf->base);
    buf->base = NULL;
    buf->len = 0;
  }
}

char *uv_buf_grow(uv_buf_t *buf, int diff) {
  if (!buf->len) {
    buf->base = malloc(diff);
    buf->len = diff;
    return buf->base;
  }

  int prev_len = buf->len;
  buf->len = prev_len + diff;
  buf->base = realloc(buf->base, buf->len);
  return buf->base + prev_len;
}

char *uv_buf_cat(uv_buf_t *buf, char *str) {
  return uv_buf_ncat(buf, str, strlen(str));
}

char *uv_buf_ncat(uv_buf_t *buf, char *str, int strlen) {
  if (!buf->len) {
    buf->base = malloc(strlen);
    buf->len = strlen;
    memcpy(buf->base, str, strlen);
    return buf->base;
  }

  int prev_len = buf->len;
  buf->len += strlen;
  buf->base = realloc(buf->base, buf->len);
  memcpy(buf->base + prev_len, str, strlen);
  return buf->base;
}

char *uv_buf_xcat(uv_buf_t *buf, char *format, ...) {
  char str[16 * 1024];
  va_list arglist;
  va_start(arglist, format);
  int rc = vsnprintf(str, sizeof(str) / sizeof(str[0]), format, arglist);
  va_end(arglist);
  return uv_buf_ncat(buf, str, rc);
}

void uv_buf_catfile(uv_buf_t *buf, char *fname) {
  FILE *f = fopen(fname, "rb");
  fseek(f, 0, SEEK_END);
  long fsize = ftell(f);
  fseek(f, 0, SEEK_SET);

  char *dst = uv_buf_grow(buf, fsize);
  fread(dst, fsize, 1, f);
  fclose(f);
}
