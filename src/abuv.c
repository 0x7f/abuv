#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "getopt.h"
#include "http_parser.h"
#include "stopwatch.h"

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
void prepare_http_request();

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
  char *host;
  char *path;
  char *port;
  char *http_req;
  int http_req_len;
  int keep_alive;
  char *user_agent;
  char *url;
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
  global->user_agent = DEFAULT_USER_AGENT;
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
  uv_buf_t buf = {.base = s_global.http_req, .len = s_global.http_req_len};
  return uv_write(&cctx->write_req, handle, &buf, 1, on_write_end);
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
    fprintf(stderr, "Unable to resolve the address \"%s\": %s\n", s_global.host,
            uv_err_name(status));
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

int parse_url(char *url) {
  struct http_parser_url parser;
  http_parser_url_init(&parser);
  int rc = http_parser_parse_url(url, strlen(url), 0, &parser);
  if (rc == 0) {
    s_global.host = extract_url_field(url, &parser, UF_HOST, NULL);
    s_global.port = extract_url_field(url, &parser, UF_PORT, "80");
    s_global.path = extract_url_field(url, &parser, UF_PATH, "/");
  }
  return rc;
}

void prepare_http_request() {
  // TODO: allow specifying custom header
  char *HTTP_REQ_FMT = "GET %s HTTP/1.1\r\n"
                       "Host: %s\r\n"
                       "User-Agent: %s\r\n"
                       "Connection: %s\r\n"
                       "\r\n";

  char *keep_alive_str = s_global.keep_alive ? "keep-alive" : "close";
  size_t req_buf_len = strlen(HTTP_REQ_FMT) + strlen(s_global.path) +
                       strlen(s_global.host) + strlen(s_global.user_agent) +
                       strlen(keep_alive_str) - (4 * strlen("%s")) + 1;
  s_global.http_req = (char *)malloc(req_buf_len);
  snprintf(s_global.http_req, req_buf_len, HTTP_REQ_FMT, s_global.path,
           s_global.host, s_global.user_agent, keep_alive_str);
  s_global.http_req_len = strlen(s_global.http_req);
}

void print_help(FILE *f, int argc, char **argv, char *err) {
  if (err) {
    fprintf(f, "Error: %s\n", err);
  }
  fprintf(f, "Usage: %s [options] http://hostname[:port][/path]\n", argv[0]);
  fprintf(f, "Options are:\n");
  fprintf(f, "    -n requests     Number of requests to perform\n");
  fprintf(f, "    -c concurrency  Number of multiple requests to make\n");
  fprintf(f, "    -t threads      Number of worker threads to spawn\n");
  fprintf(f, "    -k              Use HTTP KeepAlive feature (default)\n");
  fprintf(f, "    -K              Disable HTTP KeepAlive feature\n");
  fprintf(f, "    -h              Display usage information(this message)\n");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    print_help(stderr, argc, argv, "Too few arguments.");
    return EXIT_FAILURE;
  }

  global_ctx_defaults(&s_global);

  int c;
  opterr = 0;
  while ((c = getopt(argc, argv, "hkKn:c:t:")) != -1) {
    switch (c) {
    case 'h':
      print_help(stdout, argc, argv, NULL);
      return EXIT_SUCCESS;
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

  s_global.url = argv[argc - 1];
  if (parse_url(s_global.url) < 0) {
    fprintf(stderr, "Unable to parse url %s.\n", s_global.url);
    return EXIT_FAILURE;
  }

  prepare_http_request();

  uv_loop_t *loop = uv_default_loop();

  struct addrinfo hints;
  hints.ai_family = PF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = 0;

  uv_getaddrinfo_t resolver;
  int rc = uv_getaddrinfo(loop, &resolver, on_resolved, s_global.host,
                          s_global.port, &hints);
  if (rc < 0) {
    fprintf(stderr, "Unable to resolve %s: %s\n", s_global.host,
            uv_err_name(rc));
    return EXIT_FAILURE;
  }
  return uv_run(loop, UV_RUN_DEFAULT);
}
