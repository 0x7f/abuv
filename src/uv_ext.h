#pragma once

#include <uv.h>

uv_buf_t uv_buf_new();
void uv_buf_reset(uv_buf_t *buf);
char *uv_buf_grow(uv_buf_t *buf, int diff);
char *uv_buf_cat(uv_buf_t *buf, char *str);
char *uv_buf_ncat(uv_buf_t *buf, char *str, int strlen);
char *uv_buf_xcat(uv_buf_t *buf, char *format, ...);
void uv_buf_catfile(uv_buf_t *buf, char *fname);
