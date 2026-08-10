// Seed BaseChatMesh client: load SPIFFS identity, RX private DM, sparse TX.
#include "mc_client.h"
#include "mc_target.h"

#include <Mesh.h>
#include <SPIFFS.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/BaseChatMesh.h>
#include <helpers/IdentityStore.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#ifndef LORA_FREQ
#define LORA_FREQ 869.618f
#endif
#ifndef LORA_BW
#define LORA_BW 62.5f
#endif
#ifndef LORA_SF
#define LORA_SF 8
#endif
#ifndef LORA_CR
#define LORA_CR 5
#endif
#ifndef LORA_TX_POWER
#define LORA_TX_POWER 22
#endif

#ifndef MAX_CONTACTS
#define MAX_CONTACTS 32
#endif

#define MESH_ID_PATH "/mesh_identity.id"
#define MESH_PAIR_PATH "/mesh_pair.json"

#define SEND_TIMEOUT_BASE_MILLIS 500
#define FLOOD_SEND_TIMEOUT_FACTOR 16.0f
#define DIRECT_SEND_PERHOP_FACTOR 6.0f
#define DIRECT_SEND_PERHOP_EXTRA_MILLIS 250

static void (*cb_dm)(const char *, const char *) = nullptr;
static void (*cb_ack)(uint32_t) = nullptr;
static void (*cb_log)(const char *) = nullptr;

static void mlog(const char *fmt, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.printf("[mesh] %s\n", buf);
  if (cb_log) cb_log(buf);
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t n) {
  if (!hex || strlen(hex) < n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    char a = hex[i * 2], b = hex[i * 2 + 1];
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int hi = nib(a), lo = nib(b);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

class SeedMesh : public BaseChatMesh {
  uint32_t expected_ack_crc = 0;
  uint32_t last_msg_sent_ms = 0;
  char node_name[32] = "Pager";
  char heltec_pk_hex[65] = "";
  ContactInfo *gateway = nullptr;

  float getAirtimeBudgetFactor() const override { return 1.0f; }
  int calcRxDelay(float score, uint32_t air_time) const override {
    (void)score;
    (void)air_time;
    return 0;
  }
  bool allowPacketForward(const mesh::Packet *packet) override {
    (void)packet;
    return false;  // pager is not a repeater
  }

  void onDiscoveredContact(ContactInfo &contact, bool is_new, uint8_t path_len,
                           const uint8_t *path) override {
    (void)path;
    mlog("advert %s new=%d path_len=%u", contact.name, (int)is_new,
         (unsigned)path_len);
    uint8_t want[32];
    if (hex_to_bytes(heltec_pk_hex, want, 32) &&
        memcmp(want, contact.id.pub_key, 32) == 0) {
      gateway = lookupContactByPubKey(contact.id.pub_key, 32);
      mlog("gateway contact updated path_len=%d", (int)contact.out_path_len);
    }
  }

  void onContactPathUpdated(const ContactInfo &contact) override {
    mlog("path %s len=%d", contact.name, (int)contact.out_path_len);
    if (gateway && memcmp(gateway->id.pub_key, contact.id.pub_key, 32) == 0)
      gateway = lookupContactByPubKey(contact.id.pub_key, 32);
  }

  ContactInfo *processAck(const uint8_t *data) override {
    // BaseChatMesh only clears txt_send_timeout when processAck returns NON-NULL.
    // Returning NULL after a match still fires onSendTimeout later.
    if (expected_ack_crc && memcmp(data, &expected_ack_crc, 4) == 0) {
      uint32_t rtt = mesh_ms_clock.getMillis() - last_msg_sent_ms;
      expected_ack_crc = 0;
      mlog("ACK rtt=%lu", (unsigned long)rtt);
      if (cb_ack) cb_ack(rtt);
      if (gateway) return gateway;
      // Fall back: any contact so timeout is cancelled
      if (getNumContacts() > 0) {
        static ContactInfo tmp;
        if (getContactByIdx(0, tmp)) return &contacts[0];
      }
      return nullptr;
    }
    return nullptr;
  }

  void onMessageRecv(const ContactInfo &from, mesh::Packet *pkt,
                     uint32_t sender_timestamp, const char *text) override {
    (void)pkt;
    (void)sender_timestamp;
    mlog("DM from %s: %.40s", from.name, text ? text : "");
    if (cb_dm) cb_dm(from.name, text ? text : "");
  }

  void onCommandDataRecv(const ContactInfo &, mesh::Packet *, uint32_t,
                         const char *) override {}
  void onSignedMessageRecv(const ContactInfo &, mesh::Packet *, uint32_t,
                           const uint8_t *, const char *) override {}
  void onChannelMessageRecv(const mesh::GroupChannel &, mesh::Packet *,
                            uint32_t, const char *) override {}

  uint8_t onContactRequest(const ContactInfo &, uint32_t, const uint8_t *,
                           uint8_t, uint8_t *) override {
    return 0;
  }
  void onContactResponse(const ContactInfo &, const uint8_t *,
                         uint8_t) override {}

  uint32_t calcFloodTimeoutMillisFor(uint32_t pkt_airtime_millis) const override {
    return SEND_TIMEOUT_BASE_MILLIS +
           (uint32_t)(FLOOD_SEND_TIMEOUT_FACTOR * pkt_airtime_millis);
  }
  uint32_t calcDirectTimeoutMillisFor(uint32_t pkt_airtime_millis,
                                      uint8_t path_len) const override {
    return SEND_TIMEOUT_BASE_MILLIS +
           (uint32_t)(DIRECT_SEND_PERHOP_FACTOR * pkt_airtime_millis +
                      DIRECT_SEND_PERHOP_EXTRA_MILLIS * path_len);
  }
  void onSendTimeout() override { mlog("send timeout"); }

public:
  SeedMesh(mesh::Radio &radio, mesh::RNG &rng, mesh::RTCClock &rtc,
           mesh::PacketManager &mgr, SimpleMeshTables &tables)
      : BaseChatMesh(radio, mesh_ms_clock, rng, rtc, mgr, tables) {}

  bool start() {
    Mesh::begin();
    // SPIFFS already mounted by seed main — do not SPIFFS.begin(true) here
    // (format-on-fail would wipe wifi/token/identity).

    // Load pair meta for Heltec pubkey
    if (SPIFFS.exists(MESH_PAIR_PATH)) {
      File f = SPIFFS.open(MESH_PAIR_PATH, "r");
      if (f) {
        String s = f.readString();
        f.close();
        // crude extract "heltec_public_key":"..."
        int p = s.indexOf("heltec_public_key");
        if (p >= 0) {
          int q = s.indexOf('"', p + 18);
          if (q >= 0) {
            int q2 = s.indexOf('"', q + 1);
            if (q2 > q && (q2 - q - 1) == 64)
              s.substring(q + 1, q2).toCharArray(heltec_pk_hex, sizeof(heltec_pk_hex));
          }
        }
        p = s.indexOf("\"name\"");
        if (p >= 0) {
          int q = s.indexOf('"', p + 6);
          if (q >= 0) {
            int q2 = s.indexOf('"', q + 1);
            if (q2 > q && (q2 - q) < 32)
              s.substring(q + 1, q2).toCharArray(node_name, sizeof(node_name));
          }
        }
      }
    }

    // Identity: /mesh_identity.id is pub(32)+prv(64)
    if (!SPIFFS.exists(MESH_ID_PATH)) {
      mlog("no %s", MESH_ID_PATH);
      return false;
    }
    {
      File f = SPIFFS.open(MESH_ID_PATH, "r");
      if (!f || !self_id.readFrom(f)) {
        mlog("identity read fail");
        if (f) f.close();
        return false;
      }
      f.close();
    }

    // Sync RTC from NTP if available
    time_t now = time(nullptr);
    if (now > 1700000000) fallback_clock.setCurrentTime((uint32_t)now);

    // Seed gateway contact (flood path until we hear advert / get path)
    if (heltec_pk_hex[0]) {
      uint8_t pk[32];
      if (hex_to_bytes(heltec_pk_hex, pk, 32)) {
        ContactInfo c;
        memset(&c, 0, sizeof(c));
        c.id = mesh::Identity(pk);
        snprintf(c.name, sizeof(c.name), "Heltec-GW");
        c.type = ADV_TYPE_CHAT;
        c.flags = 0;
        c.out_path_len = OUT_PATH_UNKNOWN;  // flood until path known
        c.last_advert_timestamp = 0;
        c.lastmod = 0;
        c.shared_secret_valid = false;
        if (addContact(c)) {
          gateway = lookupContactByPubKey(pk, 32);
          mlog("gateway contact seeded");
        }
      }
    }

    mlog("self %s ready", node_name);
    // Quiet advert so Heltec can discover us (one-shot, not spammy)
    auto pkt = createSelfAdvert(node_name);
    if (pkt) sendFlood(pkt, 800);
    return true;
  }

  bool sendToGateway(const char *text, uint32_t *exp_ack, uint32_t *est_to) {
    if (!gateway) {
      // re-resolve
      uint8_t pk[32];
      if (hex_to_bytes(heltec_pk_hex, pk, 32))
        gateway = lookupContactByPubKey(pk, 32);
    }
    if (!gateway) {
      mlog("no gateway contact");
      return false;
    }
    uint32_t ack = 0, timeout = 0;
    int result =
        sendMessage(*gateway, fallback_clock.getCurrentTime(), 0, text, ack,
                    timeout);
    if (result == MSG_SEND_FAILED) {
      mlog("send failed");
      return false;
    }
    expected_ack_crc = ack;
    last_msg_sent_ms = mesh_ms_clock.getMillis();
    if (exp_ack) *exp_ack = ack;
    if (est_to) *est_to = timeout;
    mlog("sent %s (%s)", result == MSG_SEND_SENT_FLOOD ? "FLOOD" : "DIRECT",
         text);
    return true;
  }

  uint32_t lastSendMs() const { return last_msg_sent_ms; }

  void tick() {
    // keep RTC moving
    fallback_clock.tick();
    BaseChatMesh::loop();
    // refresh NTP occasionally
    static uint32_t last_sync = 0;
    uint32_t nowm = millis();
    if (nowm - last_sync > 60000) {
      last_sync = nowm;
      time_t now = time(nullptr);
      if (now > 1700000000) fallback_clock.setCurrentTime((uint32_t)now);
    }
  }
};

static SimpleMeshTables tables;
static StaticPoolPacketManager packet_mgr(16);
static SeedMesh *g_client = nullptr;
static bool g_ready = false;

void mesh_client_set_callbacks(void (*on_dm)(const char *, const char *),
                               void (*on_ack)(uint32_t),
                               void (*on_log)(const char *)) {
  cb_dm = on_dm;
  cb_ack = on_ack;
  cb_log = on_log;
}

bool mesh_client_begin() {
  if (g_ready) return true;
  if (!mesh_radio_init()) return false;
  mesh_rng.begin(mesh_radio_rng_seed());
  g_client = new SeedMesh(mesh_get_radio(), mesh_rng, fallback_clock, packet_mgr,
                          tables);
  if (!g_client->start()) {
    delete g_client;
    g_client = nullptr;
    return false;
  }
  mesh_radio_set_params(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR);
  mesh_radio_set_tx_power(LORA_TX_POWER);
  g_ready = true;
  mlog("client up");
  return true;
}

void mesh_client_loop() {
  if (g_client) g_client->tick();
}

bool mesh_client_ready() { return g_ready; }

bool mesh_client_send_to_gateway(const char *text, uint32_t *expected_ack,
                                 uint32_t *est_timeout_ms) {
  if (!g_client || !g_ready) return false;
  return g_client->sendToGateway(text, expected_ack, est_timeout_ms);
}

uint32_t mesh_client_last_send_ms() {
  return g_client ? g_client->lastSendMs() : 0;
}
