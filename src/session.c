#include "modx/session.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "modx/util.h"

int cookie_get(const char *header, const char *name, char *out, size_t outcap) {
    if (!header || !outcap) return 0;
    size_t nl = strlen(name);
    const char *p = header;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';') p++;
        if (!*p) break;
        const char *k = p;
        while (*p && *p != '=' && *p != ';') p++;
        const char *ke = p;
        while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        if (*p != '=') continue; /* attribute without value */
        p++;
        while (*p == ' ' || *p == '\t') p++;
        const char *v = p;
        while (*p && *p != ';') p++;
        const char *ve = p;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        if (ve - v >= 2 && v[0] == '"' && ve[-1] == '"') { v++; ve--; }
        if ((size_t)(ke - k) == nl && memcmp(k, name, nl) == 0) {
            size_t vl = (size_t)(ve - v);
            if (vl + 1 > outcap) return 0;
            memcpy(out, v, vl);
            out[vl] = 0;
            return 1;
        }
    }
    return 0;
}

/* Lowercased "host:port" into out. */
static int norm_hostport(const char *hp, size_t len, const char *default_port, char *out, size_t cap) {
    if (len == 0 || len + 8 >= cap) return 0;
    for (size_t i = 0; i < len; i++) out[i] = (char)tolower((unsigned char)hp[i]);
    out[len] = 0;
    /* a port is present if there is a ':' after any IPv6 literal */
    const char *rb = strrchr(out, ']');
    const char *colon = strrchr(rb ? rb : out, ':');
    if (!colon) {
        strcat(out, ":");
        strcat(out, default_port);
    }
    return 1;
}

int origin_matches_host(const char *origin, const char *host) {
    if (!origin || !host || !*host) return 0;
    const char *sep = strstr(origin, "://");
    if (!sep) return 0; /* includes the literal "null" origin */
    const char *dport;
    size_t sl = (size_t)(sep - origin);
    if (sl == 4 && strncasecmp(origin, "http", 4) == 0) dport = "80";
    else if (sl == 5 && strncasecmp(origin, "https", 5) == 0) dport = "443";
    else return 0;
    const char *hp = sep + 3;
    size_t hl = strcspn(hp, "/?#");
    char a[300], b[300];
    if (!norm_hostport(hp, hl, dport, a, sizeof a)) return 0;
    if (!norm_hostport(host, strlen(host), dport, b, sizeof b)) return 0;
    return strcmp(a, b) == 0;
}

int content_type_is_json(const char *ct) {
    if (!ct) return 0;
    while (*ct == ' ') ct++;
    if (strncasecmp(ct, "application/json", 16) != 0) return 0;
    const char *rest = ct + 16;
    while (*rest == ' ' || *rest == '\t') rest++;
    return *rest == 0 || *rest == ';';
}

void rl_init(ratelimit *rl) { rl->entries = NULL; rl->n = 0; rl->cap = 0; }
void rl_free(ratelimit *rl) { free(rl->entries); rl_init(rl); }

static rl_entry *rl_find(ratelimit *rl, const char *ip) {
    for (size_t i = 0; i < rl->n; i++)
        if (strcmp(rl->entries[i].ip, ip) == 0) return &rl->entries[i];
    return NULL;
}

static int recent_fails(const rl_entry *e, int64_t now) {
    int k = 0;
    for (int i = 0; i < e->count; i++)
        if (now - e->fails[i] < MX_RL_WINDOW_MS) k++;
    return k;
}

int rl_blocked(ratelimit *rl, const char *ip, int64_t now) {
    rl_entry *e = rl_find(rl, ip);
    return e && recent_fails(e, now) >= MX_RL_MAX_FAILS;
}

void rl_fail(ratelimit *rl, const char *ip, int64_t now) {
    rl_entry *e = rl_find(rl, ip);
    if (!e) {
        /* drop entries whose failures have all aged out before growing */
        size_t k = 0;
        for (size_t i = 0; i < rl->n; i++)
            if (recent_fails(&rl->entries[i], now) > 0) rl->entries[k++] = rl->entries[i];
        rl->n = k;
        if (rl->n == rl->cap) {
            rl->cap = rl->cap ? rl->cap * 2 : 16;
            rl->entries = xrealloc(rl->entries, rl->cap * sizeof *rl->entries);
        }
        e = &rl->entries[rl->n++];
        memset(e, 0, sizeof *e);
        snprintf(e->ip, sizeof e->ip, "%s", ip);
    }
    e->fails[e->next] = now;
    e->next = (e->next + 1) % MX_RL_MAX_FAILS;
    if (e->count < MX_RL_MAX_FAILS) e->count++;
}

void rl_reset(ratelimit *rl, const char *ip) {
    rl_entry *e = rl_find(rl, ip);
    if (e) { e->count = 0; e->next = 0; }
}

int xff_rightmost(const char *header, char *out, size_t outcap) {
    if (!header || !outcap) return 0;
    const char *comma = strrchr(header, ',');
    const char *s = comma ? comma + 1 : header;
    while (*s == ' ' || *s == '\t') s++;
    const char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    size_t n = (size_t)(e - s);
    if (n == 0 || n + 1 > outcap || n > 45) return 0;
    for (const char *p = s; p < e; p++)
        if (!isxdigit((unsigned char)*p) && *p != '.' && *p != ':') return 0;
    memcpy(out, s, n);
    out[n] = 0;
    return 1;
}

#include "modx/crypto.h"

int session_token_new(char token[65], char token_hash[65]) {
    unsigned char raw[32];
    if (crypto_random(raw, sizeof raw) != 0) return -1;
    hex_encode(raw, sizeof raw, token);
    session_token_hash(token, token_hash);
    return 0;
}

void session_token_hash(const char *token, char token_hash[65]) {
    unsigned char h[SHA256_LEN];
    sha256(token, strlen(token), h);
    hex_encode(h, sizeof h, token_hash);
}
