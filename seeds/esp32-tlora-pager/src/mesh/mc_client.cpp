// Seed BaseChatMesh client: load identity (NVS-first, mint if absent), RX
// private DM, sparse TX.
#include "mc_client.h"
#include "mc_target.h"
#include "contact_record.h"
#include "../mesh_secret_bridge.h"   // NVS get/put for the mesh identity (no heavy headers)

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

static void (*cb_dm)(const uint8_t *, const char *, const char *) = nullptr;
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

static void bytes_to_hex(const uint8_t *in, size_t n, char *out) {
  static const char H[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[i * 2] = H[(in[i] >> 4) & 0xF];
    out[i * 2 + 1] = H[in[i] & 0xF];
  }
  out[n * 2] = '\0';
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
    /* ONLY WHEN THE CONTACT IS NEW. onDiscoveredContact fires for every advert,
     * including repeats from peers already in the table (BaseChatMesh falls
     * through to the callback after updating), so saving unconditionally
     * rewrites the whole file every few minutes for as long as anything is in
     * range — erase-and-write amplification on the internal flash, and a
     * multi-millisecond blocking write on the loop task, both for data that did
     * not change. A path change is a real change and is saved separately. */
    if (is_new) saveContacts();
  }

  void onContactPathUpdated(const ContactInfo &contact) override {
    mlog("path %s len=%d", contact.name, (int)contact.out_path_len);
    if (gateway && memcmp(gateway->id.pub_key, contact.id.pub_key, 32) == 0)
      gateway = lookupContactByPubKey(contact.id.pub_key, 32);
    saveContacts();   /* the route to a peer is worth as much as the peer */
  }

 public:
  /* Write the table to /contacts3 in the stock MeshCore layout — see
   * contact_record.h for the transcribed field order. Anonymous/transient
   * entries are skipped, matching upstream's save_filter. */
  void saveContacts() {
    File f = mesh_contacts_fs().open(MESH_CONTACTS_PATH, "w");
    if (!f) { mlog("contacts save: open failed"); return; }
    int written = 0;
    for (int i = 0; i < num_contacts; i++) {
      const ContactInfo &c = contacts[i];
      if (!mesh_contact_worth_saving(c.type)) continue;
      MeshContactRec r;
      memset(&r, 0, sizeof(r));
      memcpy(r.pub_key, c.id.pub_key, MESH_CONTACT_PUBKEY_LEN);
      memcpy(r.name, c.name, MESH_CONTACT_NAME_LEN);
      r.type = c.type;
      r.flags = c.flags;
      r.sync_since = c.sync_since;
      r.out_path_len = c.out_path_len;
      r.last_advert_timestamp = c.last_advert_timestamp;
      memcpy(r.out_path, c.out_path, MESH_CONTACT_PATH_LEN);
      r.lastmod = c.lastmod;
      r.gps_lat = c.gps_lat;
      r.gps_lon = c.gps_lon;
      uint8_t buf[MESH_CONTACT_REC_LEN];
      mesh_contact_pack(&r, buf);
      if (f.write(buf, sizeof(buf)) != sizeof(buf)) break;  /* card full */
      written++;
    }
    f.close();
    mlog("contacts saved: %d", written);
  }

  /* Read the table back at boot. A short read is end-of-file, exactly as
   * upstream treats it — there is no count and no terminator. */
  int loadContacts() {
    if (!mesh_contacts_fs().exists(MESH_CONTACTS_PATH)) return 0;
    File f = mesh_contacts_fs().open(MESH_CONTACTS_PATH, "r");
    if (!f) return 0;
    int loaded = 0;
    uint8_t buf[MESH_CONTACT_REC_LEN];
    while (f.read(buf, sizeof(buf)) == (int)sizeof(buf)) {
      MeshContactRec r;
      mesh_contact_unpack(buf, &r);
      ContactInfo c;
      memset(&c, 0, sizeof(c));
      c.id = mesh::Identity(r.pub_key);
      memcpy(c.name, r.name, MESH_CONTACT_NAME_LEN);
      c.name[MESH_CONTACT_NAME_LEN - 1] = '\0';
      c.type = r.type;
      c.flags = r.flags;
      c.sync_since = r.sync_since;
      c.out_path_len = r.out_path_len;
      c.last_advert_timestamp = r.last_advert_timestamp;
      memcpy(c.out_path, r.out_path, MESH_CONTACT_PATH_LEN);
      c.lastmod = r.lastmod;
      c.gps_lat = r.gps_lat;
      c.gps_lon = r.gps_lon;
      /* Derived, never stored: recomputed from the key pair on first use. */
      c.shared_secret_valid = false;
      if (!addContact(c)) break;    /* table full */
      loaded++;
    }
    f.close();
    mlog("contacts loaded: %d", loaded);
    return loaded;
  }

 private:

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
    /* The public key is the sender's identity — the mesh attributes it —
     * while the name is only what it calls itself. Both go up; the caller
     * decides with the key and displays with the name. */
    if (cb_dm) cb_dm(from.id.pub_key, from.name, text ? text : "");
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

  // The mesh identity is a 96-byte MeshCore blob: pub(32) followed by prv(64),
  // exactly the layout LocalIdentity::writeTo(Stream&) puts in /mesh_identity.id
  // and the boot migration mirrors VERBATIM into the NVS key "mesh_id". Both
  // stores therefore hold the same bytes; this is the single serialization the
  // load and the persist below agree on, so a round-trip never moves the key.
  static const size_t MESH_ID_BLOB = 96;   // PUB_KEY_SIZE(32) + PRV_KEY_SIZE(64)

  // Write self_id to NVS (always) and, when write_file, to /mesh_identity.id.
  // NVS is the format-safe source of truth; the file is kept for the status
  // reader (meshcore.cpp) and pre-migration back-compat.
  void persistMeshIdentity(bool write_file) {
    uint8_t blob[MESH_ID_BLOB];
    memcpy(blob, self_id.pub_key, 32);          // pub first (file/Stream layout)
    uint8_t tmp[MESH_ID_BLOB];
    size_t w = self_id.writeTo(tmp, sizeof(tmp));  // buffer overload gives prv(64)+pub(32)
    if (w < 64) { mlog("identity serialize fail"); return; }
    memcpy(blob + 32, tmp, 64);                 // prv follows
    if (!mesh_secret_put("mesh_id", blob, MESH_ID_BLOB))
      mlog("identity NVS write fail");
    if (write_file) {
      File f = SPIFFS.open(MESH_ID_PATH, "w");
      if (f) { self_id.writeTo(f); f.close(); }  // Stream overload: pub+prv
      else mlog("identity file write fail");
    }
  }

  // NVS-first identity bring-up with SPIFFS fallback and mint-if-absent. RNS
  // already self-generates; mesh now does too, using the same MeshCore keygen
  // (LocalIdentity(RNG*)) the vendor lib uses, seeded from radio noise in
  // mesh_client_begin() before start() runs.
  bool bringUpIdentity() {
    // 1) NVS: the source of truth after the boot migration.
    uint8_t blob[MESH_ID_BLOB];
    size_t n = mesh_secret_get("mesh_id", blob, sizeof(blob));
    if (n == MESH_ID_BLOB) {
      char pub_hex[65], prv_hex[129];
      bytes_to_hex(blob, 32, pub_hex);
      bytes_to_hex(blob + 32, 64, prv_hex);
      self_id = mesh::LocalIdentity(prv_hex, pub_hex);
      mlog("identity from NVS");
      return true;
    }
    // 2) Pre-migration fallback: the SPIFFS file. Mirror it into NVS for next boot.
    if (SPIFFS.exists(MESH_ID_PATH)) {
      File f = SPIFFS.open(MESH_ID_PATH, "r");
      if (f && self_id.readFrom(f)) {          // Stream overload: reads pub+prv
        f.close();
        persistMeshIdentity(false);            // file already exists; NVS only
        mlog("identity from SPIFFS (mirrored to NVS)");
        return true;
      }
      if (f) f.close();
      mlog("identity read fail");
      return false;   // present but unreadable: do not overwrite by minting
    }
    // 3) Nothing anywhere: mint a fresh keypair and persist it.
    self_id = mesh::LocalIdentity(getRNG());
    persistMeshIdentity(true);                  // NVS + file
    mlog("minted mesh identity");
    return true;
  }

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

    // Identity: NVS-first, SPIFFS fallback, mint if absent (see bringUpIdentity).
    if (!bringUpIdentity()) return false;

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

  /* Is this key already an advert-attributed contact? */
  bool knowsPeer(const uint8_t *pk) {
    return pk && lookupContactByPubKey((uint8_t *)pk, 32) != nullptr;
  }

  /* Private DM to a PEER, addressed by its own public key.
   *
   * Deliberately not sendToGateway with a different argument: that function
   * resolves the one paired gateway and would silently deliver a private reply
   * to a third party if handed a peer's key. This resolves the PEER's contact
   * and sends to that, or refuses. There is no create-on-miss — a contact is
   * how the mesh says it has cryptographically attributed this key, and
   * inventing one from an unverified key is precisely the hole. */
  bool sendToPeer(const uint8_t *pk, const char *text, const char **why) {
    if (why) *why = nullptr;
    if (!pk || !text || !text[0]) {
      if (why) *why = "nothing to send";
      return false;
    }
    ContactInfo *peer = lookupContactByPubKey((uint8_t *)pk, 32);
    if (!peer) {
      if (why) *why = "unknown peer";
      return false;
    }
    /* ONE private send may await an ACK at a time, and the gateway's
     * multi-part chat holds that slot while it runs. Waiting is right here:
     * stomping it would abandon a half-delivered message the user already
     * believes they sent. The room shows the reason and the user can retry. */
    if (expected_ack_crc != 0) {
      if (why) *why = "radio busy";
      return false;
    }
    uint32_t ack = 0, timeout = 0;
    int result = sendMessage(*peer, fallback_clock.getCurrentTime(), 0, text,
                             ack, timeout);
    if (result == MSG_SEND_FAILED) {
      if (why) *why = "send failed";
      return false;
    }
    expected_ack_crc = ack;
    last_msg_sent_ms = mesh_ms_clock.getMillis();
    mlog("sent peer %s (%s)", result == MSG_SEND_SENT_FLOOD ? "FLOOD" : "DIRECT",
         peer->name);
    return true;
  }

  uint32_t lastSendMs() const { return last_msg_sent_ms; }
  bool ackPending() const { return expected_ack_crc != 0; }
  void cancelPendingAck() { expected_ack_crc = 0; }

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

void mesh_client_set_callbacks(void (*on_dm)(const uint8_t *, const char *,
                                             const char *),
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
  /* BEFORE g_ready, deliberately. Everything that sends checks
   * mesh_client_ready() first, and POST /agents/send does it from the AsyncTCP
   * task — so setting ready first would open a window where a reply could be
   * accepted while the table was still empty and be refused as "unknown peer"
   * for a peer the device knows perfectly well. Loading first makes "ready"
   * mean "ready", rather than "nearly". */
  g_client->loadContacts();
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

/* SPIFFS, not the conversation store's filesystem: the contact table is device
 * identity, it is small, and it must be there before the SD card is even
 * probed. The accessor exists so the one other reader cannot pick differently. */
fs::FS &mesh_contacts_fs() { return SPIFFS; }

void mesh_contacts_save() {
  if (g_client && g_ready) g_client->saveContacts();
}

int mesh_contacts_load() {
  return (g_client && g_ready) ? g_client->loadContacts() : 0;
}

bool mesh_client_send_to_peer(const uint8_t *pubkey, const char *text,
                              const char **why) {
  if (!g_client || !g_ready) {
    if (why) *why = "radio down";
    return false;
  }
  return g_client->sendToPeer(pubkey, text, why);
}

bool mesh_client_knows_peer(const uint8_t *pubkey) {
  return g_client && g_ready && g_client->knowsPeer(pubkey);
}

bool mesh_client_ack_pending() {
  return g_ready && g_client && g_client->ackPending();
}

void mesh_client_cancel_pending_ack() {
  if (g_client) g_client->cancelPendingAck();
}

uint32_t mesh_client_last_send_ms() {
  return g_client ? g_client->lastSendMs() : 0;
}
