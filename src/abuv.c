#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "getopt.h"
#include "http_parser.h"
#include "stopwatch.h"
#include "uv_ext.h"

#ifndef DEFAULT_USER_AGENT
#define DEFAULT_USER_AGENT "abuv/0.1"
#endif

/* forward declarations */

typedef struct conn_ctx_s conn_ctx_t;
typedef struct thread_ctx_s thread_ctx_t;
typedef struct global_ctx_s global_ctx_t;

void conn_ctx_init(conn_ctx_t *cctx, thread_ctx_t *tctx, int id);
void thread_ctx_init(thread_ctx_t *tctx, int id, uv_barrier_t *barrier);
void global_ctx_defaults(global_ctx_t *global);
void global_ctx_derived(global_ctx_t *global);

void on_alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
void on_read(uv_stream_t *server, ssize_t nread, const uv_buf_t *buf);
void on_write_end(uv_write_t *req, int status);
void on_connect(uv_connect_t *req, int status);
int write_http_request(uv_stream_t *handle);
void setup_connection(conn_ctx_t *cctx);
void on_close_conn(uv_handle_t *handle);
void on_thread(void *data);
int on_status(http_parser *parser, const char *at, size_t length);
int on_message_complete(http_parser *parser);
void run_benchmark();
void on_resolved(uv_getaddrinfo_t *resolver, int status, struct addrinfo *addr);
char *extract_url_field(const char *url, struct http_parser_url *u,
                        enum http_parser_url_fields field, char *fallback);
int parse_url(char *url, char **host, char **port, char **path);
uv_buf_t build_http_request(char *method, char *host, char *path,
                            char *user_agent, int keep_alive, uv_buf_t headers,
                            uv_buf_t body);

/* Contexts */

struct conn_ctx_s {
  int id;
  thread_ctx_t *tctx;
  uv_tcp_t socket;
  uv_connect_t connect_req;
  uv_write_t write_req;
  char *read_buf;
  size_t read_buf_size;
  struct http_parser parser;
  struct http_parser_settings parser_settings;
  int failed;
  uint64_t num_req;
  uint64_t num_req_done;
  uint64_t num_req_ok;
  uint64_t num_req_not_ok;
  uint64_t num_req_keep_alive;
  // TODO: collect per-request details e.g. for calculating p95 response times
};

struct thread_ctx_s {
  int id;
  int success;
  uv_barrier_t *barrier;
  uv_loop_t loop;
  uv_thread_t thread;
  int num_conn;
  conn_ctx_t *conn_ctxs;
};

struct global_ctx_s {
  uv_buf_t http_req;
  int keep_alive;
  uint32_t num_threads;
  uint64_t num_req;
  uint64_t num_req_per_conn;
  uint32_t num_conn;
  uint32_t num_conn_per_thread;
  struct addrinfo *addr;
};

static global_ctx_t s_global;

void conn_ctx_init(conn_ctx_t *cctx, thread_ctx_t *tctx, int id) {
  cctx->id = id;
  cctx->tctx = tctx;
  cctx->num_req = s_global.num_req_per_conn;
  cctx->parser_settings.on_status = on_status;
  cctx->parser_settings.on_message_complete = on_message_complete;
  http_parser_init(&cctx->parser, HTTP_RESPONSE);
  cctx->parser.data = cctx;
}

void thread_ctx_init(thread_ctx_t *tctx, int id, uv_barrier_t *barrier) {
  uint32_t i;
  tctx->id = id;
  tctx->barrier = barrier;
  tctx->num_conn = s_global.num_conn_per_thread;
  tctx->conn_ctxs =
      (conn_ctx_t *)calloc(sizeof(conn_ctx_t), s_global.num_conn_per_thread);
  for (i = 0; i < s_global.num_conn_per_thread; ++i) {
    conn_ctx_init(&tctx->conn_ctxs[i], tctx, i);
  }
}

void global_ctx_defaults(global_ctx_t *global) {
  global->num_req = 1;
  global->num_conn = 1;
  global->num_threads = 1;
  global->keep_alive = 1;
}

void global_ctx_derived(global_ctx_t *global) {
  s_global.num_conn_per_thread =
      (uint32_t)ceil(s_global.num_conn / s_global.num_threads);
  s_global.num_req_per_conn =
      (uint64_t)ceil(s_global.num_req / s_global.num_conn);
}

/* event callbacks */

void on_alloc_buffer(uv_handle_t *handle, size_t suggested_size,
                     uv_buf_t *buf) {
  conn_ctx_t *cctx = (conn_ctx_t *)handle->data;

  if (!cctx->read_buf) {
    cctx->read_buf = malloc(suggested_size);
    cctx->read_buf_size = suggested_size;
  }

  if (cctx->read_buf_size < suggested_size) {
    cctx->read_buf = realloc(cctx->read_buf, suggested_size);
    cctx->read_buf_size = suggested_size;
  }

  buf->base = cctx->read_buf;
  buf->len = cctx->read_buf_size;
}

void on_read(uv_stream_t *server, ssize_t nread, const uv_buf_t *buf) {
  conn_ctx_t *cctx = (conn_ctx_t *)server->data;

  if (nread == UV_EOF) {
    uv_close((uv_handle_t *)server, on_close_conn);
    return;
  }

  if (nread < 0) {
    fprintf(stderr, "error on_read: %zd %s", nread, uv_err_name(nread));
    cctx->failed = 1; // TODO: abort thread/process?
    return;
  }

  ssize_t parsed = http_parser_execute(&cctx->parser, &cctx->parser_settings,
                                       buf->base, nread);
  if (parsed < nread) {
    fprintf(stderr, "HTTP parsing error: %s\n", http_errno_name(parsed));
    return;
  }
}

void on_write_end(uv_write_t *req, int status) {
  if (status < 0) {
    fprintf(stderr, "error on_write_end: %s\n", uv_err_name(status));
    conn_ctx_t *cctx = (conn_ctx_t *)req->handle->data;
    cctx->failed = 1; // TODO: abort thread/process?
    return;
  }
}

void on_connect(uv_connect_t *req, int status) {
  int rc;
  uv_stream_t *handle = req->handle;
  conn_ctx_t *cctx = (conn_ctx_t *)handle->data;

  if (status < 0) {
    fprintf(stderr, "Error on_connect: %s\n", uv_err_name(status));
    cctx->failed = 1; // TODO: abort thread/process?
    return;
  }

  rc = write_http_request(handle);
  if (rc != 0) {
    fprintf(stderr, "Error uv_write: %d - %s\n", rc, uv_strerror(rc));
    cctx->failed = 1; // TODO: abort thread/process?
    return;
  }

  rc = uv_read_start(handle, on_alloc_buffer, on_read);
  if (rc != 0) {
    fprintf(stderr, "Error uv_read_start: %d - %s\n", rc, uv_strerror(rc));
    cctx->failed = 1; // TODO: abort thread/process?
    return;
  }
}

int write_http_request(uv_stream_t *handle) {
  conn_ctx_t *cctx = (conn_ctx_t *)handle->data;
  return uv_write(&cctx->write_req, handle, &s_global.http_req, 1,
                  on_write_end);
}

void setup_connection(conn_ctx_t *cctx) {
  int rc;
  thread_ctx_t *tctx = cctx->tctx;

  rc = uv_tcp_init(&tctx->loop, &cctx->socket);
  if (rc < 0) {
    fprintf(stderr, "[Thread#%d] error while setting up the socket: %s\n",
            tctx->id, uv_err_name(rc));
    return;
  }

  // store connection context in socket's data pointer
  cctx->socket.data = cctx;

  int keep_alive_delay = 60;
  uv_tcp_keepalive(&cctx->socket, s_global.keep_alive, keep_alive_delay);

  const struct sockaddr *addr = (const struct sockaddr *)s_global.addr->ai_addr;
  rc = uv_tcp_connect(&cctx->connect_req, &cctx->socket, addr, on_connect);
  if (rc < 0) {
    fprintf(stderr, "[Thread#%d] failed to connect to host: %s\n", tctx->id,
            uv_err_name(rc));
    cctx->failed = 1; // TODO: abort thread/process?
    return;
  }
}

// TODO: handle case where server does not support keep-alive but keep-alive
//       was requested client-side. also count & print such requests.
void on_close_conn(uv_handle_t *handle) {
  conn_ctx_t *cctx = (conn_ctx_t *)handle->data;
  if (!s_global.keep_alive && cctx->num_req_done < cctx->num_req) {
    setup_connection(cctx);
    return;
  }
}

void on_thread(void *data) {
  int rc, i;
  thread_ctx_t *ctx = (thread_ctx_t *)data;

  rc = uv_loop_init(&ctx->loop);
  if (rc < 0) {
    fprintf(stderr, "[Thread#%d] failed to init the uv loop: %s\n", ctx->id,
            uv_err_name(rc));
    return;
  }

  rc = uv_barrier_wait(ctx->barrier);
  if (rc < 0) {
    fprintf(stderr, "[Thread#%d] failed to wait on barrier: %s\n", ctx->id,
            uv_err_name(rc));
    return;
  }

  for (i = 0; i < ctx->num_conn; ++i) {
    setup_connection(&ctx->conn_ctxs[i]);
  }

  uv_run(&ctx->loop, UV_RUN_DEFAULT);
}

int on_status(http_parser *parser, const char *at, size_t length) {
  conn_ctx_t *cctx = (conn_ctx_t *)parser->data;
  if (length == 2 && strncmp("OK", at, 2) == 0) {
    ++cctx->num_req_ok;
  } else {
    ++cctx->num_req_not_ok;
  }
  return 0;
}

int on_message_complete(http_parser *parser) {
  conn_ctx_t *cctx = (conn_ctx_t *)parser->data;
  uint64_t curr_req = ++cctx->num_req_done;
  if (curr_req >= cctx->num_req) {
    uv_close((uv_handle_t *)&cctx->socket, &on_close_conn);
    return 0;
  }

  // TODO: check whether response is really a keep-alive response
  //       e.g. it is not if "HTTP 1.0" or "Connection: close"
  if (s_global.keep_alive) {
    ++cctx->num_req_keep_alive;
    write_http_request((uv_stream_t *)&cctx->socket);
  }

  return 0;
}

void on_resolved(uv_getaddrinfo_t *resolver, int status,
                 struct addrinfo *addr) {
  if (status < 0) {
    fprintf(stderr, "Unable to resolve the address: %s\n", uv_err_name(status));
    exit(EXIT_FAILURE);
  }

  s_global.addr = addr;

  char addr_str[17] = {'\0'};
  uv_ip4_name((struct sockaddr_in *)addr->ai_addr, addr_str, 16);
  printf("Host resolved to %s\n", addr_str);

  run_benchmark();
}

/* benchmark */

void run_benchmark() {
  int rc;
  uint32_t i;

  printf("Setting up %d worker threads\n", s_global.num_threads);

  uv_barrier_t barrier;
  rc = uv_barrier_init(&barrier, s_global.num_threads + 1);
  if (rc < 0) {
    fprintf(stderr, "Failed to set up the startup barrier: %s\n",
            uv_err_name(rc));
    exit(EXIT_FAILURE);
  }

  thread_ctx_t *contexts =
      (thread_ctx_t *)calloc(sizeof(thread_ctx_t), s_global.num_threads);
  for (i = 0; i < s_global.num_threads; ++i) {
    thread_ctx_init(&contexts[i], i, &barrier);
    uv_thread_create(&contexts[i].thread, &on_thread, &contexts[i]);
  }

  printf("Starting the benchmark\n");
  rc = uv_barrier_wait(&barrier);
  if (rc < 0) {
    fprintf(stderr, "uv_barrier_wait failed: %d\n", rc);
    uv_barrier_destroy(&barrier);
    exit(EXIT_FAILURE);
  }

  stopwatch_t stopwatch;
  stopwatch_start(&stopwatch);

  for (i = 0; i < s_global.num_threads; ++i) {
    uv_thread_join(&contexts[i].thread);
  }

  double elapsed = stopwatch_elapsed(&stopwatch);
  fprintf(stdout, "Benchmark done. Here are the results: %f sec, %f req/sec\n",
          elapsed, s_global.num_req / elapsed);
}

/* initialization */

char *extract_url_field(const char *url, struct http_parser_url *u,
                        enum http_parser_url_fields field, char *fallback) {
  uint16_t off = u->field_data[field].off;
  uint16_t len = u->field_data[field].len;
  if (len == 0) {
    return fallback;
  }
  char *res = (char *)malloc(len + 1);
  memcpy(res, url + off, len);
  res[len] = 0;
  return res;
}

int parse_url(char *url, char **host, char **port, char **path) {
  struct http_parser_url parser;
  http_parser_url_init(&parser);
  int rc = http_parser_parse_url(url, strlen(url), 0, &parser);
  if (rc == 0) {
    *host = extract_url_field(url, &parser, UF_HOST, NULL);
    *port = extract_url_field(url, &parser, UF_PORT, "80");
    *path = extract_url_field(url, &parser, UF_PATH, "/");
  }
  return rc;
}

uv_buf_t build_http_request(char *method, char *host, char *path,
                            char *user_agent, int keep_alive, uv_buf_t headers,
                            uv_buf_t body) {
  uv_buf_t http_req = uv_buf_new();
  uv_buf_xcat(&http_req, "%s %s HTTP/1.1\r\n", method, path);
  uv_buf_xcat(&http_req, "User-Agent: %s\r\n", user_agent);
  char *connection = keep_alive ? "keep-alive" : "close";
  uv_buf_xcat(&http_req, "Connection: %s\r\n", connection);
  if (body.len) {
    uv_buf_xcat(&http_req, "Content-Length: %d\r\n", body.len);
  }
  uv_buf_ncat(&http_req, headers.base, headers.len);
  uv_buf_cat(&http_req, "\r\n");
  if (body.len) {
    uv_buf_ncat(&http_req, body.base, body.len);
  }

  return http_req;
}

void print_opt(FILE *f, char c, char *arg, char *desc) {
  fprintf(f, "    %c%c %-12s %s\n", c ? '-' : ' ', c ? c : ' ', arg, desc);
}

void print_flag(FILE *f, char c, char *desc) { print_opt(f, c, "", desc); }

void print_add_line(FILE *f, char *desc) { print_opt(f, 0, "", desc); }

void print_help(FILE *f, int argc, char **argv, char *err) {
  if (err) {
    fprintf(f, "Error: %s\n", err);
  }
  fprintf(f, "Usage: %s [options] http://hostname[:port][/path]\n", argv[0]);
  fprintf(f, "Options are:\n");
  print_opt(f, 'n', "requests", "Number of requests to perform");
  print_opt(f, 'c', "concurrency", "Number of multiple requests to make");
  print_opt(f, 't', "threads", "Number of worker threads to spawn");
  print_opt(f, 'H', "header",
            "Add Arbitrary header line, eg. 'Accept-Encoding: gzip'");
  print_add_line(f, "Inserted after all normal header lines. (repeatable)");
  print_opt(f, 'p', "postfile",
            "File containing data to POST. Remember also to set -T");
  print_opt(f, 'u', "putfile",
            "File containing data to PUT. Remember also to set -T");
  print_opt(f, 'T', "content-type",
            "Content-type header to use for POST/PUT data, eg.");
  print_add_line(f, "'application/x-www-form-urlencoded'");
  print_add_line(f, "Default is 'text/plain'");
  print_flag(f, 'i', "Use HEAD instead of GET");
  print_flag(f, 'k', "Use HTTP KeepAlive feature (default)");
  print_flag(f, 'K', "Disable HTTP KeepAlive feature");
  print_opt(f, 'm', "method", "Method name");
  print_flag(f, 'h', "Display usage information(this message)");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    print_help(stderr, argc, argv, "Too few arguments.");
    return EXIT_FAILURE;
  }

  global_ctx_defaults(&s_global);

  char *method = "GET";
  char *host = NULL;
  char *port = NULL;
  char *path = NULL;
  char *user_agent = DEFAULT_USER_AGENT;
  uv_buf_t headers = uv_buf_init(NULL, 0);
  uv_buf_t body = uv_buf_init(NULL, 0);

  int c;
  opterr = 0;
  while ((c = getopt(argc, argv, "hH:m:p:u:T:kKin:c:t:")) != -1) {
    switch (c) {
    case 'h':
      print_help(stdout, argc, argv, NULL);
      return EXIT_SUCCESS;
    case 'H':
      uv_buf_xcat(&headers, "%s\r\n", optarg);
      break;
    case 'm':
      method = optarg;
      break;
    case 'p':
      method = "POST";
      uv_buf_catfile(&body, optarg);
      break;
    case 'u':
      method = "PUT";
      uv_buf_catfile(&body, optarg);
      break;
    case 'T':
      uv_buf_xcat(&headers, "Content-Type: %s\r\n", optarg);
      break;
    case 'i':
      method = "HEAD";
      break;
    case 'k':
      s_global.keep_alive = 1;
      break;
    case 'K':
      s_global.keep_alive = 0;
      break;
    case 'n':
      s_global.num_req = atoi(optarg);
      break;
    case 'c':
      s_global.num_conn = atoi(optarg);
      break;
    case 't':
      s_global.num_threads = atoi(optarg);
      break;
    default:
      print_help(stderr, argc, argv, "invalid argument");
      return EXIT_FAILURE;
    }
  }

  global_ctx_derived(&s_global);

  if (optind + 1 != argc) {
    print_help(stderr, argc, argv, "Exactly one url must be provided.");
    return EXIT_FAILURE;
  }

  char *url = argv[argc - 1];
  if (parse_url(url, &host, &port, &path) < 0) {
    fprintf(stderr, "Unable to parse url %s.\n", url);
    return EXIT_FAILURE;
  }

  s_global.http_req = build_http_request(method, host, path, user_agent,
                                         s_global.keep_alive, headers, body);

  uv_loop_t *loop = uv_default_loop();

  struct addrinfo hints = {0};
  hints.ai_family = PF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = 0;

  uv_getaddrinfo_t resolver;
  int rc = uv_getaddrinfo(loop, &resolver, on_resolved, host, port, &hints);
  if (rc < 0) {
    fprintf(stderr, "Unable to resolve %s: %s\n", host, uv_err_name(rc));
    return EXIT_FAILURE;
  }

  return uv_run(loop, UV_RUN_DEFAULT);
}
