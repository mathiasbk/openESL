/* http.c - the AP's HTTP server: /api/... to esl_api_handle() (as ap/main/http_api.c does
 * on the C6), the dashboard packed from web/ at /dashboard and /ui/..., and / redirects
 * to the dashboard. One thread per connection, at most MAX_CONN of them (more get 503),
 * one request per connection (Connection: close). The head and the body each have a
 * deadline for the whole of them, not per read. The API runs one request at a time under
 * api_mutex, into memory; the reply is sent after the mutex is let go, so a client that
 * reads slowly holds up nobody else. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include "ap.h"
#include "webui.h"

#define HEAD_MAX 8192
#define URI_MAX 512
#define BODY_MAX (1024 * 1024)        /* a picture (ESL_API_MAX_IMAGE_BODY) or tag firmware in one body */
#define REPLY_BUF (64 * 1024)         /* a larger reply (GET /api/tags) is collected chunk by chunk */
#define MAX_CONN 32
#define HEAD_S 10                     /* the whole head */
#define BODY_BYTES_PER_S 16384        /* the body: HEAD_S plus this rate, so 1 MB gets 74 s */
#define SEND_S 30

static pthread_mutex_t api_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned conns;

static bool send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/* The API's statuses, and the server's own */
static const char *status_line(int status)
{
    switch (status) {
    case 302: return "302 Found";
    case 411: return "411 Length Required";
    case 414: return "414 URI Too Long";
    case 431: return "431 Request Header Fields Too Large";
    case 503: return "503 Service Unavailable";
    default: return esl_api_status_line(status);
    }
}

static bool send_head(int fd, int status, const char *type, long len, const char *extra)
{
    char h[512];
    const char *line = status_line(status);
    int n = snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: %s\r\n%s%sConnection: close\r\n", line,
                     type, extra ? extra : "", "Cache-Control: no-cache\r\n");
    if (len >= 0)
        n += snprintf(h + n, sizeof(h) - (size_t)n, "Content-Length: %ld\r\n", len);
    n += snprintf(h + n, sizeof(h) - (size_t)n, "\r\n");
    return send_all(fd, h, (size_t)n);
}

static void send_text(int fd, int status, const char *msg)
{
    char b[256];
    int n = snprintf(b, sizeof(b), "{\"error\":\"%s\"}", msg);
    if (send_head(fd, status, "application/json", n, NULL))
        send_all(fd, b, (size_t)n);
}

/* ---- API replies: collected in memory under api_mutex, sent after it ---- */

typedef struct {
    char *data;
    size_t len, cap;
    bool failed;
} reply_t;

static bool reply_add(reply_t *r, const char *p, size_t n)
{
    if (r->len + n > r->cap) {
        size_t cap = r->cap ? r->cap * 2 : REPLY_BUF;
        while (cap < r->len + n)
            cap *= 2;
        char *d = realloc(r->data, cap);
        if (!d) {
            r->failed = true;
            return false;
        }
        r->data = d;
        r->cap = cap;
    }
    memcpy(r->data + r->len, p, n);
    r->len += n;
    return true;
}

static bool reply_sink(esl_sb_t *sb)
{
    return reply_add(sb->ctx, sb->buf, sb->len);
}

static void api_request(int fd, const char *method, const char *uri, const char *auth, const char *body, size_t len)
{
    static char *buf; /* under api_mutex */
    esl_sb_t out;
    reply_t r = {0};
    int status;

    pthread_mutex_lock(&api_mutex);
    if (!buf && !(buf = malloc(REPLY_BUF))) {
        pthread_mutex_unlock(&api_mutex);
        send_text(fd, 500, "no memory");
        return;
    }
    esl_sb_init(&out, buf, REPLY_BUF);
    esl_sb_set_sink(&out, reply_sink, &r);
    status = esl_api_handle(&ap.api, method, uri, auth, body, len, ap_now_ms(), &out);
    if (out.sent) {
        /* a streamed reply: its chunks are in r, the rest in buf */
        if (status != out.status || out.overflow)
            r.failed = true;
        else
            reply_add(&r, out.buf, out.len);
    } else if (!out.overflow) {
        reply_add(&r, out.buf, out.len);
    } else {
        r.failed = true;
    }
    pthread_mutex_unlock(&api_mutex);

    if (r.failed) {
        ap_log("%s %s: reply failed (%d)", method, uri, status);
        send_text(fd, 500, "reply too large");
    } else if (send_head(fd, status, "application/json", (long)r.len, NULL)) {
        send_all(fd, r.data, r.len);
    }
    free(r.data);
    ap_debug("%s %s -> %d", method, uri, status);
}

static void webui_request(int fd, const char *path)
{
    for (const webui_file_t *f = webui_files; f->path; f++) {
        if (strcmp(f->path, path) != 0)
            continue;
        if (send_head(fd, 200, f->type, (long)f->len, "Content-Encoding: gzip\r\n"))
            send_all(fd, f->gz, f->len);
        return;
    }
    send_text(fd, 404, "not found");
}

/* ---- one connection ---- */

static const char *header(char *head, const char *name)
{
    size_t n = strlen(name);
    for (char *l = strstr(head, "\r\n"); l && l[2]; l = strstr(l + 2, "\r\n")) {
        char *h = l + 2;
        if (strncasecmp(h, name, n) == 0 && h[n] == ':') {
            h += n + 1;
            while (*h == ' ' || *h == '\t')
                h++;
            return h;
        }
    }
    return NULL;
}

/* Copies a header value up to its line end */
static void header_copy(char *head, const char *name, char *out, size_t cap)
{
    const char *v = header(head, name);
    size_t i = 0;
    while (v && v[i] && v[i] != '\r' && i + 1 < cap) {
        out[i] = v[i];
        i++;
    }
    out[i] = 0;
}

/* recv() with what is left of deadline (monotonic us): -1 when it has passed */
static ssize_t recv_until(int fd, void *buf, size_t n, int64_t deadline)
{
    int64_t left = deadline - ap_clock_us();
    if (left <= 0)
        return -1;
    struct timeval tv = {(time_t)(left / 1000000), (suseconds_t)(left % 1000000)};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return recv(fd, buf, n, 0);
}

static void request(int fd)
{
    char *head = malloc(HEAD_MAX + 1);
    char *body = NULL;
    size_t got = 0;
    int64_t deadline = ap_clock_us() + HEAD_S * 1000000LL;

    if (!head)
        return;
    /* the head, up to the empty line */
    char *end = NULL;
    while (!end) {
        if (got == HEAD_MAX) {
            send_text(fd, 431, "request head too large");
            goto done;
        }
        ssize_t n = recv_until(fd, head + got, HEAD_MAX - got, deadline);
        if (n <= 0)
            goto done;
        got += (size_t)n;
        head[got] = 0;
        end = strstr(head, "\r\n\r\n");
    }
    size_t head_len = (size_t)(end - head) + 4;
    end[2] = 0; /* the headers end with one CRLF now */

    char method[8] = "", uri[URI_MAX] = "";
    size_t ml = strcspn(head, " "), ul = strcspn(head + ml + 1, " \r");
    if (ml == 0 || ml >= sizeof(method) || head[ml] != ' ') {
        send_text(fd, 400, "bad request");
        goto done;
    }
    if (ul == 0 || ul >= sizeof(uri)) {
        send_text(fd, ul ? 414 : 400, ul ? "URI too long" : "bad request");
        goto done;
    }
    memcpy(method, head, ml);
    memcpy(uri, head + ml + 1, ul);
    char clen[24], te[32], auth[ESL_API_MAX_TOKENS + 16];
    header_copy(head, "Content-Length", clen, sizeof(clen));
    header_copy(head, "Transfer-Encoding", te, sizeof(te));
    header_copy(head, "Authorization", auth, sizeof(auth));
    if (te[0]) {
        send_text(fd, 411, "send the body with Content-Length");
        goto done;
    }
    long len = 0;
    if (clen[0]) {
        if (strspn(clen, "0123456789") != strlen(clen) || strlen(clen) > 9) {
            send_text(fd, strlen(clen) > 9 && strspn(clen, "0123456789") == strlen(clen) ? 413 : 400,
                      "bad Content-Length");
            goto done;
        }
        len = strtol(clen, NULL, 10);
    }
    if (len > BODY_MAX) {
        send_text(fd, 413, "body too large");
        goto done;
    }
    if (len) {
        body = malloc((size_t)len + 1);
        if (!body) {
            send_text(fd, 500, "no memory");
            goto done;
        }
        size_t have = got - head_len;
        if (have > (size_t)len)
            have = (size_t)len;
        memcpy(body, head + head_len, have);
        deadline += (int64_t)len * 1000000 / BODY_BYTES_PER_S;
        while (have < (size_t)len) {
            ssize_t n = recv_until(fd, body + have, (size_t)len - have, deadline);
            if (n <= 0)
                goto done;
            have += (size_t)n;
        }
        body[len] = 0;
    }

    char path[URI_MAX];
    snprintf(path, sizeof(path), "%s", uri);
    char *q = strchr(path, '?');
    if (q)
        *q = 0;
    if (strncmp(path, "/api/", 5) == 0) {
        api_request(fd, method, uri, auth, body, (size_t)len);
    } else if (strcmp(method, "GET") != 0) {
        send_text(fd, 405, "method not allowed");
    } else if (strcmp(path, "/") == 0) {
        send_head(fd, 302, "text/plain", 0, "Location: /dashboard\r\n");
    } else {
        webui_request(fd, path);
    }
done:
    free(body);
    free(head);
}

static void *connection(void *arg)
{
    int fd = (int)(intptr_t)arg;
    struct timeval tv = {SEND_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    request(fd);
    close(fd);
    pthread_mutex_lock(&conn_mutex);
    conns--;
    pthread_mutex_unlock(&conn_mutex);
    return NULL;
}

/* ---- the listening socket ---- */

static void *server(void *arg)
{
    int ls = (int)(intptr_t)arg;
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) {
            if (errno != EINTR)
                usleep(100000);
            continue;
        }
        pthread_mutex_lock(&conn_mutex);
        bool room = conns < MAX_CONN;
        if (room)
            conns++;
        pthread_mutex_unlock(&conn_mutex);
        if (!room) {
            /* as the C6's httpd with its socket limit: the newest one waits for nothing */
            struct timeval tv = {1, 0};
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            send_text(fd, 503, "too many connections");
            close(fd);
            continue;
        }
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&a, 256 * 1024);
        if (pthread_create(&t, &a, connection, (void *)(intptr_t)fd) != 0) {
            close(fd);
            pthread_mutex_lock(&conn_mutex);
            conns--;
            pthread_mutex_unlock(&conn_mutex);
        }
        pthread_attr_destroy(&a);
    }
    return NULL;
}

bool http_start(const char *addr)
{
    char host[256] = "";
    const char *port = strrchr(addr, ':');
    if (!port || !port[1]) {
        fprintf(stderr, "openesl-ap: --listen wants [host]:port, not \"%s\"\n", addr);
        return false;
    }
    snprintf(host, sizeof(host), "%.*s", (int)(port - addr), addr);
    port++;
    /* [::1]:8080 */
    if (host[0] == '[') {
        size_t n = strlen(host);
        if (n >= 2 && host[n - 1] == ']') {
            memmove(host, host + 1, n - 2);
            host[n - 2] = 0;
        }
    }
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int err = getaddrinfo(host[0] ? host : NULL, port, &hints, &res);
    if (err) {
        fprintf(stderr, "openesl-ap: --listen %s: %s\n", addr, gai_strerror(err));
        return false;
    }
    int ls = -1;
    /* the first address that works; with no host, the IPv6 wildcard takes IPv4 too where the
       system allows it */
    for (struct addrinfo *ai = res; ai && ls < 0; ai = ai->ai_next) {
        ls = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (ls < 0)
            continue;
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(ls, ai->ai_addr, ai->ai_addrlen) != 0 || listen(ls, 16) != 0) {
            err = errno;
            close(ls);
            ls = -1;
        }
    }
    freeaddrinfo(res);
    if (ls < 0) {
        fprintf(stderr, "openesl-ap: cannot listen on %s: %s%s\n", addr, strerror(err),
                err == EACCES ? " (a port under 1024 needs root or CAP_NET_BIND_SERVICE)" : "");
        return false;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, server, (void *)(intptr_t)ls) != 0) {
        close(ls);
        return false;
    }
    pthread_detach(t);
    ap_log("HTTP on %s", addr);
    return true;
}
