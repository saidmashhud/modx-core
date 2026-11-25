#include "modx/session.h"

#include <arpa/inet.h>

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

void rl_init(ratelimit *rl) { rl_init_cap(rl, 0); }

void rl_init_cap(ratelimit *rl, size_t max_keys) {
    memset(rl, 0, sizeof *rl);
    rl->max = max_keys;
}

void rl_free(ratelimit *rl) {
    free(rl->entries);
    free(rl->index);
    rl_init_cap(rl, rl->max);
}

static uint64_t rl_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

/* Index slot holding key, or the empty slot where it would go. */
static size_t rl_slot(const ratelimit *rl, const char *key) {
    size_t mask = rl->index_cap - 1;
    size_t i = (size_t)rl_hash(key) & mask;
    while (rl->index[i] && strcmp(rl->entries[rl->index[i] - 1].ip, key) != 0) i = (i + 1) & mask;
    return i;
}

static rl_entry *rl_find(ratelimit *rl, const char *key) {
    if (!rl->index) return NULL;
    uint32_t v = rl->index[rl_slot(rl, key)];
    return v ? &rl->entries[v - 1] : NULL;
}

/* Removes the entry at dense position d (backward-shift deletion in the index,
 * the last entry moves into the hole). */
static void rl_remove_at(ratelimit *rl, size_t d) {
    size_t mask = rl->index_cap - 1;
    size_t i = rl_slot(rl, rl->entries[d].ip), j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (!rl->index[j]) break;
        size_t k = (size_t)rl_hash(rl->entries[rl->index[j] - 1].ip) & mask;
        /* the entry at j may stay if its home k lies cyclically in (i, j] */
        if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) continue;
        rl->index[i] = rl->index[j];
        i = j;
    }
    rl->index[i] = 0;
    size_t last = rl->n - 1;
    if (d != last) {
        rl->entries[d] = rl->entries[last];
        rl->index[rl_slot(rl, rl->entries[d].ip)] = (uint32_t)(d + 1);
    }
    rl->n--;
}

static int recent_fails(const rl_entry *e, int64_t now) {
    int k = 0;
    for (int i = 0; i < e->count; i++)
        if (now - e->fails[i] < MX_RL_WINDOW_MS) k++;
    return k;
}

/* What is stored for a key (truncated to fit rl_entry.ip). */
static void rl_key(const char *ip, char key[64]) { snprintf(key, 64, "%s", ip); }

int rl_blocked(ratelimit *rl, const char *ip, int64_t now) {
    char key[sizeof rl->entries[0].ip];
    rl_key(ip, key);
    rl_entry *e = rl_find(rl, key);
    return e && recent_fails(e, now) >= MX_RL_MAX_FAILS;
}

/* Full: drop every aged-out key; if that frees nothing, evict the key with
 * the oldest last failure, a blocked one only when every key is blocked. */
static void rl_make_room(ratelimit *rl, int64_t now) {
    for (size_t d = rl->n; d-- > 0;)
        if (now - rl->entries[d].last >= MX_RL_WINDOW_MS) rl_remove_at(rl, d);
    if (rl->n < rl->cap) return;
    size_t victim = 0;
    int victim_blocked = 1;
    for (size_t d = 0; d < rl->n; d++) {
        const rl_entry *e = &rl->entries[d];
        int b = recent_fails(e, now) >= MX_RL_MAX_FAILS;
        if (d == 0 || (victim_blocked && !b) || (b == victim_blocked && e->last < rl->entries[victim].last)) {
            victim = d;
            victim_blocked = b;
        }
    }
    rl_remove_at(rl, victim);
    rl->evictions++;
}

void rl_fail(ratelimit *rl, const char *ip, int64_t now) {
    if (!rl->entries) {
        rl->cap = rl->max ? rl->max : MX_RL_CAPACITY;
        if (rl->cap > UINT32_MAX / 4) rl->cap = UINT32_MAX / 4;
        rl->index_cap = 16;
        while (rl->index_cap < rl->cap * 2) rl->index_cap *= 2;
        rl->entries = xcalloc(rl->cap, sizeof *rl->entries);
        rl->index = xcalloc(rl->index_cap, sizeof *rl->index);
    }
    char key[sizeof rl->entries[0].ip];
    rl_key(ip, key);
    rl_entry *e = rl_find(rl, key);
    if (!e) {
        /* incremental sweep: a few entries per new key are checked and dropped
         * once all their failures aged out (O(1) per call, keeps the table tidy) */
        for (int k = 0; k < 4 && rl->n; k++) {
            if (rl->sweep >= rl->n) rl->sweep = 0;
            if (now - rl->entries[rl->sweep].last >= MX_RL_WINDOW_MS) rl_remove_at(rl, rl->sweep);
            else rl->sweep++;
        }
        if (rl->n == rl->cap) rl_make_room(rl, now);
        size_t slot = rl_slot(rl, key);
        e = &rl->entries[rl->n];
        memset(e, 0, sizeof *e);
        memcpy(e->ip, key, sizeof key);
        rl->index[slot] = (uint32_t)(++rl->n);
    }
    e->fails[e->next] = now;
    e->next = (e->next + 1) % MX_RL_MAX_FAILS;
    if (e->count < MX_RL_MAX_FAILS) e->count++;
    e->last = now;
}

void rl_reset(ratelimit *rl, const char *ip) {
    if (!rl->index) return;
    char key[sizeof rl->entries[0].ip];
    rl_key(ip, key);
    uint32_t v = rl->index[rl_slot(rl, key)];
    if (v) rl_remove_at(rl, v - 1);
}

int mx_ip_normalize(const char *in, char *out, size_t outcap) {
    if (!in || !outcap) return 0;
    while (*in == ' ' || *in == '\t') in++;
    size_t n = strlen(in);
    while (n && (in[n - 1] == ' ' || in[n - 1] == '\t')) n--;
    if (n == 0 || n > 63) return 0;
    char buf[64];
    memcpy(buf, in, n);
    buf[n] = 0;
    char *addr = buf;
    if (buf[0] == '[') {
        /* "[v6]" or "[v6]:port" */
        char *close = strchr(buf, ']');
        if (!close) return 0;
        if (close[1] && (close[1] != ':' || !close[2] || strspn(close + 2, "0123456789") != strlen(close + 2))) return 0;
        *close = 0;
        addr = buf + 1;
    } else {
        /* "a.b.c.d:port": exactly one colon after a dotted quad */
        char *colon = strchr(buf, ':');
        if (colon && !strchr(colon + 1, ':') && strchr(buf, '.') && strchr(buf, '.') < colon) {
            if (!colon[1] || strspn(colon + 1, "0123456789") != strlen(colon + 1)) return 0;
            *colon = 0;
        }
    }
    char *zone = strchr(addr, '%'); /* fe80::1%en0: the zone says nothing about the client */
    if (zone) *zone = 0;
    unsigned char raw[16];
    char tmp[INET6_ADDRSTRLEN];
    if (inet_pton(AF_INET, addr, raw) == 1) {
        if (!inet_ntop(AF_INET, raw, tmp, sizeof tmp)) return 0;
    } else if (inet_pton(AF_INET6, addr, raw) == 1) {
        static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
        if (memcmp(raw, mapped, 12) == 0) {
            if (!inet_ntop(AF_INET, raw + 12, tmp, sizeof tmp)) return 0;
        } else if (!inet_ntop(AF_INET6, raw, tmp, sizeof tmp)) {
            return 0;
        }
    } else {
        return 0;
    }
    if (strlen(tmp) + 1 > outcap) return 0;
    memcpy(out, tmp, strlen(tmp) + 1);
    return 1;
}

int mx_ip_rate_key(const char *ip, unsigned v6_prefix, char *out, size_t outcap) {
    char norm[64];
    if (!mx_ip_normalize(ip, norm, sizeof norm)) return 0;
    unsigned char raw[16];
    if (inet_pton(AF_INET6, norm, raw) != 1) { /* IPv4: the address itself */
        if (strlen(norm) + 1 > outcap) return 0;
        memcpy(out, norm, strlen(norm) + 1);
        return 1;
    }
    unsigned bits = v6_prefix ? v6_prefix : MX_IP_V6_PREFIX;
    if (bits > 128) bits = 128;
    for (unsigned i = 0; i < 16; i++) {
        unsigned keep = bits > i * 8 ? (bits - i * 8 >= 8 ? 8 : bits - i * 8) : 0;
        raw[i] &= (unsigned char)(0xff00u >> keep);
    }
    char tmp[INET6_ADDRSTRLEN];
    if (!inet_ntop(AF_INET6, raw, tmp, sizeof tmp)) return 0;
    char key[INET6_ADDRSTRLEN + 8];
    if (bits == 128) snprintf(key, sizeof key, "%s", tmp);
    else snprintf(key, sizeof key, "%s/%u", tmp, bits);
    if (strlen(key) + 1 > outcap) return 0;
    memcpy(out, key, strlen(key) + 1);
    return 1;
}

int xff_rightmost(const char *header, char *out, size_t outcap) {
    if (!header || !outcap) return 0;
    const char *comma = strrchr(header, ',');
    const char *s = comma ? comma + 1 : header;
    while (*s == ' ' || *s == '\t') s++;
    const char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
    size_t n = (size_t)(e - s);
    if (n == 0 || n > 63) return 0;
    char el[64];
    memcpy(el, s, n);
    el[n] = 0;
    /* an address literal (IPv4, IPv6, "[v6]:port", "v4:port", IPv4-mapped), in canonical form */
    return mx_ip_normalize(el, out, outcap);
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
