/* http.c - the AP's HTTP server: /api/... to esl_api_handle() (as ap/main/http_api.c does
 * on the C6), the dashboard packed from web/ at /dashboard and /ui/..., and / redirects
 * to the dashboard. One thread per connection, one request per connection
 * (Connection: close); the API itself runs one request at a time under api_mutex. */
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
#define BODY_MAX (1024 * 1024)        /* a picture (ESL_API_MAX_IMAGE_BODY) or tag firmware in one body */
#define REPLY_BUF (64 * 1024)         /* larger replies (GET /api/tags) are streamed */
#define TIMEOUT_S 30

static pthread_mutex_t api_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static bool send_head(int fd, int status, const char *type, long len, const char *extra)
{
    char h[512];
    /* the API's statuses, and the redirect from / */
    const char *line = status == 302 ? "302 Found" : esl_api_status_line(status);
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

/* ---- streamed API replies (esl_sb_set_sink): close-delimited, no Content-Length ---- */

typedef struct {
    int fd;
} sink_t;

static bool http_sink(esl_sb_t *sb)
{
    sink_t *s = sb->ctx;
    if (sb->sent == 0 && !send_head(s->fd, sb->status, "application/json", -1, NULL))
        return false;
    return send_all(s->fd, sb->buf, sb->len);
}

static void api_request(int fd, const char *method, const char *uri, const char *auth, const char *body, size_t len)
{
    static char *buf; /* under api_mutex */
    esl_sb_t out;
    sink_t sink = {fd};

    pthread_mutex_lock(&api_mutex);
    if (!buf && !(buf = malloc(REPLY_BUF))) {
        pthread_mutex_unlock(&api_mutex);
        send_text(fd, 500, "no memory");
        return;
    }
    esl_sb_init(&out, buf, REPLY_BUF);
    esl_sb_set_sink(&out, http_sink, &sink);
    int status = esl_api_handle(&ap.api, method, uri, auth, body, len, ap_now_ms(), &out);
    if (out.sent) {
        /* the head has gone out with out.status; nothing can replace it */
        if (status == out.status && !out.overflow)
            esl_sb_flush(&out);
        else
            ap_log("%s %s: streamed reply failed (%d)", method, uri, status);
    } else if (out.overflow) {
        send_text(fd, 500, "reply too large");
    } else if (send_head(fd, status, "application/json", (long)out.len, NULL)) {
        send_all(fd, out.buf, out.len);
    }
    pthread_mutex_unlock(&api_mutex);
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

static void *connection(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char *head = malloc(HEAD_MAX + 1);
    char *body = NULL;
    size_t got = 0;
    struct timeval tv = {TIMEOUT_S, 0};

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (!head)
        goto done;
    /* the head, up to the empty line */
    char *end = NULL;
    while (!end) {
        if (got == HEAD_MAX) {
            send_text(fd, 431, "request head too large");
            goto done;
        }
        ssize_t n = recv(fd, head + got, HEAD_MAX - got, 0);
        if (n <= 0)
            goto done;
        got += (size_t)n;
        head[got] = 0;
        end = strstr(head, "\r\n\r\n");
    }
    size_t head_len = (size_t)(end - head) + 4;
    end[2] = 0; /* the headers end with one CRLF now */

    char method[8] = "", uri[512] = "";
    if (sscanf(head, "%7s %511s", method, uri) != 2) {
        send_text(fd, 400, "bad request");
        goto done;
    }
    char clen[24], auth[ESL_API_MAX_TOKENS + 16];
    header_copy(head, "Content-Length", clen, sizeof(clen));
    header_copy(head, "Authorization", auth, sizeof(auth));
    long len = clen[0] ? strtol(clen, NULL, 10) : 0;
    if (len < 0 || len > BODY_MAX) {
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
        while (have < (size_t)len) {
            ssize_t n = recv(fd, body + have, (size_t)len - have, 0);
            if (n <= 0)
                goto done;
            have += (size_t)n;
        }
        body[len] = 0;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s", uri);
    char *q = strchr(path, '?');
    if (q)
        *q = 0;
    if (strncmp(path, "/api/", 5) == 0) {
        api_request(fd, method, uri, auth, body, (size_t)len);
    } else if (strcmp(method, "GET") != 0) {
        send_text(fd, 405, "method not allowed");
    } else if (strcmp(path, "/") == 0) {
        if (send_head(fd, 302, "text/plain", 0, "Location: /dashboard\r\n"))
            ap_debug("GET / -> /dashboard");
    } else {
        webui_request(fd, path);
    }
done:
    free(body);
    free(head);
    close(fd);
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
        pthread_t t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&a, 256 * 1024);
        if (pthread_create(&t, &a, connection, (void *)(intptr_t)fd) != 0)
            close(fd);
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
