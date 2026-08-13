#pragma once
/*
 * net_view.h — the sectioned Network-status model (pure, host-testable).
 *
 * The pager carries four independent connection layers (WiFi, Reticulum, the
 * MeshCore radio and the WireGuard tunnel), and until now the "how am I
 * connected" screen was a flat wall of up to twelve tiny text lines built ad
 * hoc in main.cpp. This header replaces the DECISIONS behind that screen with a
 * pure model that turns a per-transport status snapshot into the SAME kind of
 * sectioned row list the messenger's other screens use (see contacts_view.h):
 * an amber section header per transport, then a handful of value rows, each row
 * carrying a short label, a short value and a status LEVEL the renderer maps to
 * a glyph and a colour.
 *
 * WHAT IS PURE HERE, AND WHY. The section ORDER, WHICH rows a section shows for
 * a given transport state, how a transport that is OFF or absent collapses to a
 * single row, the glyph a status level gets, and where the row cap truncates —
 * none of that can be confirmed by a screenshot, and all of it is a decision.
 * So it is done here with no Arduino and no panel, and proved BY VALUE in
 * tools/test_net_view.cpp. Drawing (bars, colours, scroll) stays in hw_ui.cpp.
 * This is READ-ONLY status: nothing here touches connectivity behaviour.
 *
 * THE INPUT IS A TYPED SNAPSHOT, NOT PRE-FORMATTED TEXT. The device half fills
 * NetStatus from the real accessors (WiFi.*, g_rns_*, g_mesh, wg_ui_state) and
 * hands it in; the model decides the rows. Values that need a live number
 * (SSID, IP, RSSI, RF params, the address hash) are passed as strings the model
 * copies into fixed row buffers — bounded, always terminated — so the caller may
 * drop its scratch the moment net_build_rows() returns.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Status level of one value row. The ORDER is not significant; the values are a
 * closed set the glyph/colour maps switch on. OFF = the transport is not wanted
 * or absent; OK = healthy; WARN = degraded/transitional; DOWN = wanted but not
 * working. */
enum NetLevel {
    NET_LVL_OFF  = 0,
    NET_LVL_OK   = 1,
    NET_LVL_WARN = 2,
    NET_LVL_DOWN = 3,
};

/* The four sections, in the order they are drawn. The values ARE the draw
 * order. Unlike Contacts, EVERY section is always drawn — a status screen must
 * account for every transport, so an off/absent one shows one collapsed row
 * under its header rather than vanishing (which would read as "not present"). */
enum NetSection {
    NET_SEC_WIFI      = 0,
    NET_SEC_RETICULUM = 1,
    NET_SEC_MESH      = 2,
    NET_SEC_TUNNEL    = 3,
};
#define NET_SECTION_N 4

/* One row is either a section header (only kind/section/label meaningful, label
 * = the section title) or a value row (label + value + level). */
enum NetRowKind {
    NET_ROW_HEADER = 0,
    NET_ROW_VALUE  = 1,
};

/* Row field widths. Labels are one short word ("SSID"/"RSSI"/"addr"/"key"...);
 * values bound the widest live string (an IPv4, an RSSI, an 8-hex key, a short
 * address hash), and a longer SSID/hash is truncated into this, never overrun. */
#define NET_LABEL_LEN 10
#define NET_VALUE_LEN 26

/* Section titles — one word each, distinct so the sections say something. */
#define NET_TITLE_WIFI      "WIFI"
#define NET_TITLE_RETICULUM "RETICULUM"
#define NET_TITLE_MESH      "MESH"
#define NET_TITLE_TUNNEL    "TUNNEL"

/* Per-section ceiling on VALUE rows (headers are counted separately). WiFi shows
 * SSID/IP/RSSI when up; Reticulum a state + an address; Mesh a state + key + RF
 * + last-seen; the tunnel a single state line. */
#define NET_WIFI_ROWS_MAX 3
#define NET_RNS_ROWS_MAX  2
#define NET_MESH_ROWS_MAX 4
#define NET_TUN_ROWS_MAX  1

/* The defined ceiling on total rows (four headers + every section at its cap).
 * The caller's own `max` may be smaller; the model never writes more than
 * either, and never strands a header with no value row under it. */
#define NET_ROWS_MAX (NET_SECTION_N + NET_WIFI_ROWS_MAX + NET_RNS_ROWS_MAX + \
                      NET_MESH_ROWS_MAX + NET_TUN_ROWS_MAX)   /* = 14 */

/* Thresholds, pinned so a screenshot cannot silently move them. RSSI at or above
 * NET_RSSI_OK_MIN dBm reads OK, weaker reads WARN (a connected link is never
 * DOWN on strength alone). A mesh last-seen within NET_MESH_SEEN_FRESH_S reads
 * OK, older reads WARN. */
#define NET_RSSI_OK_MIN        (-75)
#define NET_MESH_SEEN_FRESH_S  180

/* --- input snapshot -------------------------------------------------------- */

struct NetWifiStatus {
    bool        wanted;     /* credentials present and not user-toggled off  */
    bool        connected;  /* WL_CONNECTED                                  */
    const char *ssid;       /* connected only; may be NULL                   */
    const char *ip;         /* connected only; may be NULL                   */
    int         rssi_dbm;   /* connected only                                */
    int         profiles;   /* saved-profile count (>=0)                     */
};

struct NetRnsStatus {
    bool        enabled;      /* configured AND the operator wants it up      */
    bool        has_identity; /* a node identity exists                       */
    bool        link_up;      /* RNS TCP link connected                       */
    const char *addr;         /* identity address hash (hex); may be NULL     */
};

struct NetMeshStatus {
    bool        has_identity; /* mesh keys present                            */
    int         ui_state;     /* MESH_UI_* : 0 OFF,1 WAIT,2 OK,3 STALE,4 DOWN */
    const char *key8;         /* local public key, short hex; may be NULL     */
    const char *rf;           /* preformatted RF line "868.0 SF11"; may be NULL */
    int         seen_age_s;   /* seconds since last alive; <0 = never         */
};

struct NetTunStatus {
    bool wanted;    /* WG configured and wanted                              */
    int  ui_state;  /* WG_UI_* : 0 OFF,1 WAIT,2 OK,3 STALE,4 DOWN           */
};

struct NetStatus {
    NetWifiStatus wifi;
    NetRnsStatus  rns;
    NetMeshStatus mesh;
    NetTunStatus  tun;
};

/* --- one drawn row --------------------------------------------------------- */

struct NetRow {
    uint8_t kind;                 /* NetRowKind                               */
    uint8_t section;              /* NetSection this row belongs to           */
    uint8_t level;                /* NetLevel (value rows; header = NET_LVL_OFF) */
    char    label[NET_LABEL_LEN]; /* value: field name; header: section title */
    char    value[NET_VALUE_LEN]; /* value row only; "" on a header           */
};

/* --- helpers --------------------------------------------------------------- */

/* The section title for a section id, or "" for an out-of-range value. */
static inline const char *net_section_title(uint8_t section) {
    switch (section) {
        case NET_SEC_WIFI:      return NET_TITLE_WIFI;
        case NET_SEC_RETICULUM: return NET_TITLE_RETICULUM;
        case NET_SEC_MESH:      return NET_TITLE_MESH;
        case NET_SEC_TUNNEL:    return NET_TITLE_TUNNEL;
        default:                return "";
    }
}

/* The glyph a status level is drawn with. Distinct per level so the eye can run
 * the column without reading the values; the renderer colours it to match. */
static inline char net_level_glyph(uint8_t level) {
    switch (level) {
        case NET_LVL_OK:   return '+';
        case NET_LVL_WARN: return '~';
        case NET_LVL_DOWN: return '!';
        default:           return '-';   /* NET_LVL_OFF */
    }
}

/* Copy a NUL-terminated field into a fixed row buffer, always terminated. */
static inline void net_copy_str(char *dst, size_t dst_n, const char *src) {
    size_t j = 0;
    if (src)
        for (; src[j] && j + 1 < dst_n; j++) dst[j] = src[j];
    dst[j] = '\0';
}

/* Emit one row, bounded by `max`. Silently a no-op once the array is full, so a
 * section that overruns `max` simply loses its tail rows rather than overrunning
 * the buffer. */
static inline void net_put(NetRow *out, int *w, int max, uint8_t kind,
                           uint8_t section, uint8_t level,
                           const char *label, const char *value) {
    if (*w >= max) return;
    NetRow &r = out[*w];
    r.kind = kind;
    r.section = section;
    r.level = level;
    net_copy_str(r.label, sizeof(r.label), label);
    net_copy_str(r.value, sizeof(r.value), value);
    (*w)++;
}

/* A section header is written ONLY when its first value row will also fit, so
 * the cap never leaves a dangling header. Returns true (and writes the header)
 * when there is room for a header + at least one value; false to skip the
 * section (and every section after it) instead. */
static inline bool net_open_section(NetRow *out, int *w, int max, uint8_t section) {
    if (*w + 2 > max) return false;
    net_put(out, w, max, NET_ROW_HEADER, section, NET_LVL_OFF,
            net_section_title(section), "");
    return true;
}

/* --- per-section builders -------------------------------------------------- */

static inline void net_build_wifi(const NetWifiStatus *s, NetRow *out,
                                  int *w, int max) {
    if (!net_open_section(out, w, max, NET_SEC_WIFI)) return;
    if (!s->wanted) {
        net_put(out, w, max, NET_ROW_VALUE, NET_SEC_WIFI, NET_LVL_OFF,
                "WiFi", "OFF");
        return;
    }
    if (!s->connected) {
        net_put(out, w, max, NET_ROW_VALUE, NET_SEC_WIFI, NET_LVL_DOWN,
                "WiFi", "offline");
        char b[NET_VALUE_LEN];
        int prof = s->profiles < 0 ? 0 : s->profiles;
        snprintf(b, sizeof(b), "%d saved", prof);
        net_put(out, w, max, NET_ROW_VALUE, NET_SEC_WIFI,
                prof > 0 ? NET_LVL_OK : NET_LVL_WARN, "saved", b);
        return;
    }
    net_put(out, w, max, NET_ROW_VALUE, NET_SEC_WIFI, NET_LVL_OK,
            "SSID", s->ssid ? s->ssid : "?");
    net_put(out, w, max, NET_ROW_VALUE, NET_SEC_WIFI, NET_LVL_OK,
            "IP", s->ip ? s->ip : "?");
    char b[NET_VALUE_LEN];
    snprintf(b, sizeof(b), "%d dBm", s->rssi_dbm);
    net_put(out, w, max, NET_ROW_VALUE, NET_SEC_WIFI,
            s->rssi_dbm >= NET_RSSI_OK_MIN ? NET_LVL_OK : NET_LVL_WARN,
            "RSSI", b);
}

static inline void net_build_rns(const NetRnsStatus *s, NetRow *out,
                                 int *w, int max) {
    if (!net_open_section(out, w, max, NET_SEC_RETICULUM)) return;
    if (!s->enabled) {
        net_put(out, w, max, NET_ROW_VALUE, NET_SEC_RETICULUM, NET_LVL_OFF,
                "RNS", "OFF");
        return;
    }
    if (s->link_up) {
        net_put(out, w, max, NET_ROW_VALUE, NET_SEC_RETICULUM, NET_LVL_OK,
                "RNS", "up");
    } else if (s->has_identity) {
        net_put(out, w, max, NET_ROW_VALUE, NET_SEC_RETICULUM, NET_LVL_WARN,
                "RNS", "down");
    } else {
        net_put(out, w, max, NET_ROW_VALUE, NET_SEC_RETICULUM, NET_LVL_DOWN,
                "RNS", "no identity");
    }
    net_put(out, w, max, NET_ROW_VALUE, NET_SEC_RETICULUM,
            s->has_identity ? NET_LVL_OK : NET_LVL_OFF,
            "addr", s->addr ? s->addr : "-");
}

static inline void net_build_mesh(const NetMeshStatus *s, NetRow *out,
                                  int *w, int max) {
    if (!net_open_section(out, w, max, NET_SEC_MESH)) return;
    if (!s->has_identity) {
        net_put(out, w, max, NET_ROW_VALUE, NET_SEC_MESH, NET_LVL_OFF,
                "Mesh", "no identity");
        return;
    }
    const char *word;
    uint8_t lvl;
    switch (s->ui_state) {
        case 2:  word = "up";    lvl = NET_LVL_OK;   break;  /* MESH_UI_OK    */
        case 3:  word = "stale"; lvl = NET_LVL_WARN; break;  /* MESH_UI_STALE */
        case 4:  word = "down";  lvl = NET_LVL_DOWN; break;  /* MESH_UI_DOWN  */
        default: word = "wait";  lvl = NET_LVL_WARN; break;  /* MESH_UI_WAIT  */
    }
    net_put(out, w, max, NET_ROW_VALUE, NET_SEC_MESH, lvl, "Mesh", word);
    net_put(out, w, max, NET_ROW_VALUE, NET_SEC_MESH,
            s->key8 ? NET_LVL_OK : NET_LVL_WARN,
            "key", s->key8 ? s->key8 : "-");
    net_put(out, w, max, NET_ROW_VALUE, NET_SEC_MESH,
            s->rf ? NET_LVL_OK : NET_LVL_OFF, "RF", s->rf ? s->rf : "-");
    if (s->seen_age_s < 0) {
        net_put(out, w, max, NET_ROW_VALUE, NET_SEC_MESH, NET_LVL_WARN,
                "seen", "never");
    } else {
        char b[NET_VALUE_LEN];
        snprintf(b, sizeof(b), "%ds ago", s->seen_age_s);
        net_put(out, w, max, NET_ROW_VALUE, NET_SEC_MESH,
                s->seen_age_s <= NET_MESH_SEEN_FRESH_S ? NET_LVL_OK : NET_LVL_WARN,
                "seen", b);
    }
}

static inline void net_build_tun(const NetTunStatus *s, NetRow *out,
                                 int *w, int max) {
    if (!net_open_section(out, w, max, NET_SEC_TUNNEL)) return;
    const char *word;
    uint8_t lvl;
    switch (s->ui_state) {
        case 2:  word = "up";    lvl = NET_LVL_OK;   break;  /* WG_UI_OK    */
        case 1:  word = "wait";  lvl = NET_LVL_WARN; break;  /* WG_UI_WAIT  */
        case 3:  word = "stale"; lvl = NET_LVL_WARN; break;  /* WG_UI_STALE */
        case 4:  word = "down";  lvl = NET_LVL_DOWN; break;  /* WG_UI_DOWN  */
        default: word = "off";   lvl = NET_LVL_OFF;  break;  /* WG_UI_OFF   */
    }
    net_put(out, w, max, NET_ROW_VALUE, NET_SEC_TUNNEL, lvl, "WG", word);
}

/*
 * Build the sectioned status row list.
 *
 * Sections are emitted in NetSection order (WiFi, Reticulum, mesh, tunnel), and
 * EVERY section that fits is drawn even when its transport is off — an off/absent
 * transport collapses to a single row under its header. The result is bounded by
 * the caller's `max` and by NET_ROWS_MAX, a header is never stranded without a
 * value row, and every row is self-contained (label/value copied in), so the
 * caller may drop its scratch the moment this returns.
 *
 * Returns the number of rows written.
 */
static inline int net_build_rows(const NetStatus *s, NetRow *out, int max) {
    if (!s || !out || max <= 0) return 0;
    if (max > NET_ROWS_MAX) max = NET_ROWS_MAX;
    int w = 0;
    net_build_wifi(&s->wifi, out, &w, max);
    net_build_rns(&s->rns, out, &w, max);
    net_build_mesh(&s->mesh, out, &w, max);
    net_build_tun(&s->tun, out, &w, max);
    return w;
}
