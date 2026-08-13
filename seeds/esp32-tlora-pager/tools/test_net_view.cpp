/*
 * Host tests for the sectioned Network-status model in src/net_view.h.
 *
 * Drawing needs the panel and eyes; SECTIONING, the per-state ROW SET, the
 * glyph a status level gets, how an OFF transport collapses, where the cap
 * truncates and how a long value is bounded are all DECISIONS a screenshot
 * cannot confirm. So these check the row list BY VALUE, and they drive the SAME
 * pure functions the device calls (net_build_rows / net_level_glyph /
 * net_section_title).
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/net_view.h"

/* A fully-up snapshot: every section shows its richest row set. Used as the
 * baseline the individual tests mutate one field at a time. */
static NetStatus all_up(void) {
    NetStatus s;
    memset(&s, 0, sizeof(s));
    s.wifi.wanted = true;
    s.wifi.connected = true;
    s.wifi.ssid = "HomeNet";
    s.wifi.ip = "192.168.1.116";
    s.wifi.rssi_dbm = -55;
    s.wifi.profiles = 2;
    s.rns.enabled = true;
    s.rns.has_identity = true;
    s.rns.link_up = true;
    s.rns.addr = "a1b2c3d4";
    s.mesh.has_identity = true;
    s.mesh.ui_state = 2;   /* MESH_UI_OK */
    s.mesh.key8 = "deadbeef";
    s.mesh.rf = "868.0 SF11";
    s.mesh.seen_age_s = 12;
    s.tun.wanted = true;
    s.tun.ui_state = 2;    /* WG_UI_OK */
    return s;
}

/* Find the header row index for a section, or -1. */
static int header_of(const NetRow *rows, int n, uint8_t section) {
    for (int i = 0; i < n; i++)
        if (rows[i].kind == NET_ROW_HEADER && rows[i].section == section) return i;
    return -1;
}

/* --- 1. sections come out in NetSection order, header then values ---------- */

static void test_section_order(void) {
    NetStatus s = all_up();
    NetRow out[NET_ROWS_MAX];
    int n = net_build_rows(&s, out, NET_ROWS_MAX);
    /* all-up == the ceiling: 4 headers + 3 wifi + 2 rns + 4 mesh + 1 tun. */
    assert(n == NET_ROWS_MAX);
    assert(n == 14);

    int hw = header_of(out, n, NET_SEC_WIFI);
    int hr = header_of(out, n, NET_SEC_RETICULUM);
    int hm = header_of(out, n, NET_SEC_MESH);
    int ht = header_of(out, n, NET_SEC_TUNNEL);
    assert(hw == 0);           /* WiFi first                                  */
    assert(hw < hr && hr < hm && hm < ht);   /* strict draw order            */

    assert(strcmp(out[hw].label, NET_TITLE_WIFI) == 0);
    assert(strcmp(out[hr].label, NET_TITLE_RETICULUM) == 0);
    assert(strcmp(out[hm].label, NET_TITLE_MESH) == 0);
    assert(strcmp(out[ht].label, NET_TITLE_TUNNEL) == 0);

    /* The row after each header is that section's first value row. */
    assert(out[hw + 1].kind == NET_ROW_VALUE && out[hw + 1].section == NET_SEC_WIFI);
    assert(strcmp(out[hw + 1].label, "SSID") == 0);
    assert(strcmp(out[hw + 1].value, "HomeNet") == 0);
    assert(out[ht + 1].kind == NET_ROW_VALUE && out[ht + 1].section == NET_SEC_TUNNEL);
    assert(strcmp(out[ht + 1].label, "WG") == 0 && strcmp(out[ht + 1].value, "up") == 0);
}

/* --- 2. per-status level derivation ---------------------------------------- */

static void test_wifi_levels(void) {
    NetRow out[NET_ROWS_MAX];

    /* Strong RSSI -> OK, weak -> WARN (never DOWN on strength while connected). */
    NetStatus s = all_up();
    s.wifi.rssi_dbm = NET_RSSI_OK_MIN;          /* exactly at the boundary   */
    int n = net_build_rows(&s, out, NET_ROWS_MAX);
    int hw = header_of(out, n, NET_SEC_WIFI);
    assert(strcmp(out[hw + 3].label, "RSSI") == 0);
    assert(out[hw + 3].level == NET_LVL_OK);

    s.wifi.rssi_dbm = NET_RSSI_OK_MIN - 1;      /* one weaker -> WARN        */
    n = net_build_rows(&s, out, NET_ROWS_MAX);
    hw = header_of(out, n, NET_SEC_WIFI);
    assert(out[hw + 3].level == NET_LVL_WARN);

    /* Offline but wanted: a DOWN state row and a saved-profiles row. */
    s = all_up();
    s.wifi.connected = false;
    s.wifi.profiles = 3;
    n = net_build_rows(&s, out, NET_ROWS_MAX);
    hw = header_of(out, n, NET_SEC_WIFI);
    assert(strcmp(out[hw + 1].value, "offline") == 0 && out[hw + 1].level == NET_LVL_DOWN);
    assert(strcmp(out[hw + 2].label, "saved") == 0);
    assert(strcmp(out[hw + 2].value, "3 saved") == 0 && out[hw + 2].level == NET_LVL_OK);

    /* Offline with no saved profiles: the saved row warns. */
    s.wifi.profiles = 0;
    n = net_build_rows(&s, out, NET_ROWS_MAX);
    hw = header_of(out, n, NET_SEC_WIFI);
    assert(strcmp(out[hw + 2].value, "0 saved") == 0 && out[hw + 2].level == NET_LVL_WARN);
}

static void test_rns_levels(void) {
    NetRow out[NET_ROWS_MAX];

    /* Link up -> OK. */
    NetStatus s = all_up();
    int n = net_build_rows(&s, out, NET_ROWS_MAX);
    int h = header_of(out, n, NET_SEC_RETICULUM);
    assert(strcmp(out[h + 1].value, "up") == 0 && out[h + 1].level == NET_LVL_OK);
    assert(strcmp(out[h + 2].label, "addr") == 0);
    assert(strcmp(out[h + 2].value, "a1b2c3d4") == 0 && out[h + 2].level == NET_LVL_OK);

    /* Enabled, has identity, not linked -> WARN "down". */
    s.rns.link_up = false;
    n = net_build_rows(&s, out, NET_ROWS_MAX);
    h = header_of(out, n, NET_SEC_RETICULUM);
    assert(strcmp(out[h + 1].value, "down") == 0 && out[h + 1].level == NET_LVL_WARN);

    /* Enabled but no identity -> DOWN, and the address row shows "-" dimmed. */
    s.rns.has_identity = false;
    s.rns.addr = NULL;
    n = net_build_rows(&s, out, NET_ROWS_MAX);
    h = header_of(out, n, NET_SEC_RETICULUM);
    assert(strcmp(out[h + 1].value, "no identity") == 0 && out[h + 1].level == NET_LVL_DOWN);
    assert(strcmp(out[h + 2].value, "-") == 0 && out[h + 2].level == NET_LVL_OFF);
}

static void test_mesh_levels(void) {
    NetRow out[NET_ROWS_MAX];

    /* ui_state maps to word+level; seen<0 warns, fresh is OK, stale warns. */
    struct { int st; const char *word; uint8_t lvl; } cases[] = {
        { 2, "up",    NET_LVL_OK   },
        { 1, "wait",  NET_LVL_WARN },
        { 3, "stale", NET_LVL_WARN },
        { 4, "down",  NET_LVL_DOWN },
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        NetStatus s = all_up();
        s.mesh.ui_state = cases[k].st;
        int n = net_build_rows(&s, out, NET_ROWS_MAX);
        int h = header_of(out, n, NET_SEC_MESH);
        assert(strcmp(out[h + 1].value, cases[k].word) == 0);
        assert(out[h + 1].level == cases[k].lvl);
        /* key / RF / seen rows follow. */
        assert(strcmp(out[h + 2].label, "key") == 0);
        assert(strcmp(out[h + 3].label, "RF") == 0);
        assert(strcmp(out[h + 4].label, "seen") == 0);
    }

    /* Fresh vs stale last-seen. */
    NetStatus s = all_up();
    s.mesh.seen_age_s = NET_MESH_SEEN_FRESH_S;      /* boundary -> OK        */
    int n = net_build_rows(&s, out, NET_ROWS_MAX);
    int h = header_of(out, n, NET_SEC_MESH);
    assert(strcmp(out[h + 4].value, "180s ago") == 0 && out[h + 4].level == NET_LVL_OK);
    s.mesh.seen_age_s = NET_MESH_SEEN_FRESH_S + 1;  /* one older -> WARN     */
    n = net_build_rows(&s, out, NET_ROWS_MAX);
    h = header_of(out, n, NET_SEC_MESH);
    assert(out[h + 4].level == NET_LVL_WARN);
    s.mesh.seen_age_s = -1;                          /* never -> WARN         */
    n = net_build_rows(&s, out, NET_ROWS_MAX);
    h = header_of(out, n, NET_SEC_MESH);
    assert(strcmp(out[h + 4].value, "never") == 0 && out[h + 4].level == NET_LVL_WARN);

    /* Missing key / RF strings collapse to "-" without inventing a value. */
    s = all_up();
    s.mesh.key8 = NULL;
    s.mesh.rf = NULL;
    n = net_build_rows(&s, out, NET_ROWS_MAX);
    h = header_of(out, n, NET_SEC_MESH);
    assert(strcmp(out[h + 2].value, "-") == 0 && out[h + 2].level == NET_LVL_WARN);
    assert(strcmp(out[h + 3].value, "-") == 0 && out[h + 3].level == NET_LVL_OFF);
}

static void test_tunnel_levels(void) {
    NetRow out[NET_ROWS_MAX];
    struct { int st; const char *word; uint8_t lvl; } cases[] = {
        { 0, "off",   NET_LVL_OFF  },
        { 1, "wait",  NET_LVL_WARN },
        { 2, "up",    NET_LVL_OK   },
        { 3, "stale", NET_LVL_WARN },
        { 4, "down",  NET_LVL_DOWN },
    };
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        NetStatus s = all_up();
        s.tun.ui_state = cases[k].st;
        int n = net_build_rows(&s, out, NET_ROWS_MAX);
        int h = header_of(out, n, NET_SEC_TUNNEL);
        assert(strcmp(out[h + 1].label, "WG") == 0);
        assert(strcmp(out[h + 1].value, cases[k].word) == 0);
        assert(out[h + 1].level == cases[k].lvl);
    }
}

/* --- 3. an OFF/absent transport collapses to exactly one row --------------- */

static void test_off_sections(void) {
    NetRow out[NET_ROWS_MAX];

    /* Every section always has a header, even when its transport is off. */
    NetStatus s;
    memset(&s, 0, sizeof(s));          /* nothing wanted / no identity        */
    s.tun.ui_state = 0;                /* WG_UI_OFF                           */
    int n = net_build_rows(&s, out, NET_ROWS_MAX);
    assert(n == NET_SECTION_N * 2);    /* 4 headers + 4 single OFF rows       */

    int hw = header_of(out, n, NET_SEC_WIFI);
    assert(hw >= 0);
    assert(strcmp(out[hw + 1].value, "OFF") == 0 && out[hw + 1].level == NET_LVL_OFF);
    /* ...and no second WiFi value row: the next row is the Reticulum header. */
    assert(out[hw + 2].kind == NET_ROW_HEADER && out[hw + 2].section == NET_SEC_RETICULUM);

    int hr = header_of(out, n, NET_SEC_RETICULUM);
    assert(strcmp(out[hr + 1].value, "OFF") == 0 && out[hr + 1].level == NET_LVL_OFF);
    int hm = header_of(out, n, NET_SEC_MESH);
    assert(strcmp(out[hm + 1].value, "no identity") == 0 && out[hm + 1].level == NET_LVL_OFF);
    int ht = header_of(out, n, NET_SEC_TUNNEL);
    assert(strcmp(out[ht + 1].value, "off") == 0 && out[ht + 1].level == NET_LVL_OFF);
}

/* --- 4. the glyph mapping -------------------------------------------------- */

static void test_glyph_mapping(void) {
    assert(net_level_glyph(NET_LVL_OK)   == '+');
    assert(net_level_glyph(NET_LVL_WARN) == '~');
    assert(net_level_glyph(NET_LVL_DOWN) == '!');
    assert(net_level_glyph(NET_LVL_OFF)  == '-');
    assert(net_level_glyph(99)           == '-');   /* out of range -> OFF glyph */
    /* The four glyphs must be distinct, or the column says nothing. */
    char g[4] = { net_level_glyph(NET_LVL_OK), net_level_glyph(NET_LVL_WARN),
                  net_level_glyph(NET_LVL_DOWN), net_level_glyph(NET_LVL_OFF) };
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
            assert(g[i] != g[j]);
}

/* --- 5. the row cap: bounded, no dangling header --------------------------- */

static void test_caps(void) {
    NetStatus s = all_up();
    NetRow out[NET_ROWS_MAX];

    /* Asking for more than the ceiling clamps to NET_ROWS_MAX exactly. */
    NetRow big[NET_ROWS_MAX + 8];
    assert(net_build_rows(&s, big, 100000) == NET_ROWS_MAX);

    /* A small max truncates without stranding a header: the last row written is
     * never a lone header. */
    for (int m = 1; m <= NET_ROWS_MAX; m++) {
        int n = net_build_rows(&s, out, m);
        assert(n <= m);
        if (n > 0)
            assert(out[n - 1].kind == NET_ROW_VALUE);   /* tail is a value    */
        /* max == 1 cannot fit header + value, so nothing is written. */
        if (m == 1) assert(n == 0);
    }
}

/* --- 6. value bounding ----------------------------------------------------- */

static void test_value_bounding(void) {
    NetRow out[NET_ROWS_MAX];
    NetStatus s = all_up();

    /* An SSID far wider than the value field is cut and stays terminated. */
    char longssid[NET_VALUE_LEN * 3];
    memset(longssid, 'S', sizeof(longssid) - 1);
    longssid[sizeof(longssid) - 1] = '\0';
    s.wifi.ssid = longssid;
    int n = net_build_rows(&s, out, NET_ROWS_MAX);
    int hw = header_of(out, n, NET_SEC_WIFI);
    assert(strlen(out[hw + 1].value) == NET_VALUE_LEN - 1);

    /* A long address hash is likewise bounded. */
    char longaddr[NET_VALUE_LEN * 3];
    memset(longaddr, 'a', sizeof(longaddr) - 1);
    longaddr[sizeof(longaddr) - 1] = '\0';
    s = all_up();
    s.rns.addr = longaddr;
    n = net_build_rows(&s, out, NET_ROWS_MAX);
    int hr = header_of(out, n, NET_SEC_RETICULUM);
    assert(strlen(out[hr + 2].value) == NET_VALUE_LEN - 1);
}

/* --- 7. the section-title helper ------------------------------------------- */

static void test_section_title(void) {
    assert(strcmp(net_section_title(NET_SEC_WIFI), NET_TITLE_WIFI) == 0);
    assert(strcmp(net_section_title(NET_SEC_RETICULUM), NET_TITLE_RETICULUM) == 0);
    assert(strcmp(net_section_title(NET_SEC_MESH), NET_TITLE_MESH) == 0);
    assert(strcmp(net_section_title(NET_SEC_TUNNEL), NET_TITLE_TUNNEL) == 0);
    assert(net_section_title(99)[0] == '\0');
    /* The four titles must be distinct. */
    const char *t[NET_SECTION_N] = { NET_TITLE_WIFI, NET_TITLE_RETICULUM,
                                     NET_TITLE_MESH, NET_TITLE_TUNNEL };
    for (int i = 0; i < NET_SECTION_N; i++)
        for (int j = i + 1; j < NET_SECTION_N; j++)
            assert(strcmp(t[i], t[j]) != 0);
}

/* --- 8. degenerate input --------------------------------------------------- */

static void test_bounds(void) {
    NetStatus s = all_up();
    NetRow out[NET_ROWS_MAX];
    assert(net_build_rows(NULL, out, NET_ROWS_MAX) == 0);
    assert(net_build_rows(&s, NULL, NET_ROWS_MAX) == 0);
    assert(net_build_rows(&s, out, 0) == 0);
    assert(net_build_rows(&s, out, -3) == 0);
}

int main(void) {
    test_section_order();
    test_wifi_levels();
    test_rns_levels();
    test_mesh_levels();
    test_tunnel_levels();
    test_off_sections();
    test_glyph_mapping();
    test_caps();
    test_value_bounding();
    test_section_title();
    test_bounds();
    printf("net view tests: OK\n");
    return 0;
}
