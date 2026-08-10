/*
 * skills/rns.cpp — Reticulum stack bring-up (microReticulum 0.5.0)
 *
 * C1 is a cost measurement, not a feature: the stack is instantiated and an
 * identity is brought up on SPIFFS, but NO interface is registered, nothing is
 * announced and the SX1262 is never touched (MeshCore still owns the radio).
 * `Reticulum::start()` has no interface check, so a zero-interface stack is a
 * supported configuration.
 *
 * NO tick: with zero interfaces `Reticulum::loop()` has nothing useful to do
 * (empty interface list, no-op filesystem loop, jobs over empty tables) and two
 * things it should not do on our loop task — it catches std::bad_alloc itself
 * and calls ESP.restart() unconditionally (no caller can intercept that), and
 * RNG.loop() does a synchronous NVS erase+write of the entropy seed on an hour
 * timer. The tick gets wired in by the commit that registers a real interface.
 *
 * Filesystem: microStore's UniversalFileSystem resolves to the POSIX adapter on
 * ESP32 and would mount LittleFS; this board is SPIFFS, so the SPIFFS adapter
 * is selected via -DUSTORE_USE_SPIFFS. init() is called with reformatOnFail
 * FALSE on purpose — the default true does a probe write and calls
 * SPIFFS.format() if it fails, which would take /mesh_identity.id,
 * /gw_token.txt, /wg.json and the whole notify store with it. The adapter also
 * calls SPIFFS.begin(true, "") itself, so init() only runs once SPIFFS is
 * confirmed mounted: mounting with an empty base path and formatOnFail set is
 * the same partition loss by another route.
 *
 * Order matters: OS::storage_size()/storage_available() are called
 * unconditionally by start() and throw std::runtime_error when no filesystem is
 * registered, so registration comes first and the whole bring-up sits in a
 * try/catch (an escaping exception is a panic, not a degraded skill).
 *
 * Identity (X25519 + Ed25519, 64 private bytes) is stored as hex text at
 * /rns_identity.id through main.cpp's atomic tmp+rename writer, matching how
 * meshcore.cpp keeps /mesh_identity.id. Generation is gated on SPIFFS.exists():
 * a file that is present but unreadable or malformed is reported, never
 * overwritten.
 *
 * Endpoint:
 *   GET /rns/status — {ready, identity, hash, mem:{...}}; degrades to
 *                     ready:false with a reason instead of disappearing.
 */

#include <microStore/Adapters/SPIFFSFileSystem.h>
#include <microReticulum.h>

#define RNS_ID_PATH  "/rns_identity.id"
#define RNS_ID_TMP   "/rns_identity.tmp"
/* Identity::get_private_key() is X25519(32) + Ed25519(32). */
#define RNS_PRV_KEY_BYTES  (RNS::Type::Identity::KEYSIZE / 8)
/* Truncated hash is 16 bytes today; the buffer holds a full 32-byte hex too. */
#define RNS_HEXHASH_MAX    65

/* ---- state ---- */
static bool rns_started = false;         /* Reticulum::start() returned */
static bool rns_identity_ok = false;
static bool rns_identity_created = false; /* generated this boot, not loaded */
static const char *rns_error = nullptr;   /* static literal, or NULL when fine */
static char rns_hexhash[RNS_HEXHASH_MAX] = {0};

/* File scope for lifetime clarity, not because a local would dangle:
 * OS::register_filesystem() stores a COPY of the wrapper and the wrapper holds
 * a shared_ptr to the adapter implementation, so the implementation would
 * survive a local going out of scope. Keeping the wrapper here means the one
 * we call init() on and the one the library holds are the same object. */
static microStore::FileSystem rns_filesystem{microStore::Adapters::SPIFFSFileSystem()};
static RNS::Reticulum rns_stack{RNS::Type::NONE};
static RNS::Identity rns_identity{RNS::Type::NONE};

/* ---- identity ---- */

/* Bytes::assignHex() validates NOTHING: its digit arithmetic maps every input
 * byte to some value and the only length handling is a truncation to an even
 * count, so a corrupted ASCII byte would decode to a different key instead of
 * failing. Every character is checked before it reaches the decoder. */
static bool rns_is_hex(const String &s) {
    for (size_t i = 0; i < s.length(); i++) {
        if (!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}

/* Load /rns_identity.id, or mint one when the file does not exist. The branch
 * is decided by SPIFFS.exists(), never by content: read_spiffs_file() returns
 * "" both for an absent file and for a failed open/read, so deciding on the
 * string would regenerate — and overwrite — a key we merely failed to read. */
static void rns_identity_bring_up() {
    if (SPIFFS.exists(RNS_ID_PATH)) {
        String hex = read_spiffs_file(RNS_ID_PATH);
        hex.trim();
        if (hex.length() == 0) {
            rns_error = "stored identity is present but unreadable";
            return;
        }
        if (hex.length() != RNS_PRV_KEY_BYTES * 2) {
            rns_error = "stored identity has the wrong length";
            return;
        }
        if (!rns_is_hex(hex)) {
            rns_error = "stored identity is not hex";
            return;
        }
        RNS::Bytes prv;
        prv.assignHex(hex.c_str());
        if (prv.size() != RNS_PRV_KEY_BYTES) {
            rns_error = "stored identity failed to decode";
            return;
        }
        RNS::Identity loaded(false);      /* false: do not generate keys */
        if (!loaded.load_private_key(prv)) {
            rns_error = "stored identity rejected by the stack";
            return;
        }
        rns_identity = loaded;
    } else {
        RNS::Identity fresh;              /* default ctor generates a keypair */
        String out = fresh.get_private_key().toHex().c_str();
        if (!write_spiffs_file_atomic(RNS_ID_PATH, RNS_ID_TMP, out)) {
            /* The key is live in RAM but will not survive a reboot; say so
             * rather than pretending the identity is persistent. */
            rns_error = "identity generated but could not be saved";
        }
        rns_identity = fresh;
        rns_identity_created = true;
    }

    rns_identity_ok = true;
    strncpy(rns_hexhash, rns_identity.hexhash().c_str(), sizeof(rns_hexhash) - 1);
    rns_hexhash[sizeof(rns_hexhash) - 1] = '\0';
}

/* ---- HTTP ---- */

static void rns_status_json(JsonDocument &doc) {
    doc["ready"] = rns_started;
    doc["identity"] = rns_identity_ok;
    if (rns_identity_ok) {
        doc["hash"] = rns_hexhash;
        doc["created"] = rns_identity_created;
    } else {
        doc["hash"] = (const char *)nullptr;
    }
    doc["transport"] = RNS::Reticulum::transport_enabled();
    doc["interfaces"] = 0;                 /* C1 registers none, by design */
    if (rns_error) doc["error"] = rns_error;

    /* The library's own counters next to the platform's, so /rns/status can be
     * diffed against /capabilities when the stack starts carrying traffic.
     * alloc_count/free_count read 0 under RNS_HEAP_ALLOCATOR — the library only
     * bumps them from the global operator new it declines to override here. */
    JsonObject mem = doc["mem"].to<JsonObject>();
    mem["rns_heap_size"] = RNS::Utilities::Memory::heap_size();
    mem["rns_heap_available"] = RNS::Utilities::Memory::heap_available();
    mem["alloc_count"] = RNS::Utilities::Memory::default_allocator_alloc();
    mem["free_count"] = RNS::Utilities::Memory::default_allocator_free();
    mem["free_heap"] = ESP.getFreeHeap();
    mem["free_psram"] = ESP.getFreePsram();
}

static const SkillEndpoint rns_endpoints[] = {
    {"GET", "/rns/status", "Reticulum stack state, identity hash and memory cost"},
    {NULL, NULL, NULL}
};

static const char *rns_describe() {
    return "## Skill: rns\n\n"
           "Reticulum (microReticulum 0.5.0) brought up alongside MeshCore.\n"
           "This is the measurement stage: the stack runs with **no interface\n"
           "registered**, transport and probe destinations disabled, and it\n"
           "never touches the SX1262 — MeshCore still owns the radio.\n"
           "The node identity (X25519 + Ed25519) lives in SPIFFS at\n"
           "`/rns_identity.id` as hex and is generated once on first boot.\n"
           "`GET /rns/status` reports the stack state, the identity hash and\n"
           "both the library's allocator counters and the ESP heap/PSRAM\n"
           "figures. `mem.alloc_count` and `mem.free_count` are expected to\n"
           "read 0: the build uses the library's heap allocator, which leaves\n"
           "the global `operator new` — and therefore the counters — alone.\n"
           "If bring-up fails the endpoint stays up and answers `ready:false`\n"
           "with a reason.\n\n"
           "| GET | /rns/status |\n";
}

static void rns_register_routes(AsyncWebServer &server) {
    server.on(AsyncURIMatcher::exact("/rns/status"), HTTP_GET,
              [](AsyncWebServerRequest *req) {
        if (!require_auth(req)) return;
        JsonDocument doc;
        rns_status_json(doc);
        notify_send_json(req, 200, doc);
    });
}

static const Skill rns_skill = {
    .name = "rns",
    .version = "0.1.0",
    .describe = rns_describe,
    .endpoints = rns_endpoints,
    .register_routes = rns_register_routes,
    /* No tick — see the header comment: an empty-interface Reticulum::loop()
     * only costs us an ESP.restart() path and an NVS write on the loop task. */
    .tick = nullptr
};

static void skill_rns_init() {
    if (rns_started) return;             /* one bring-up per boot */

    try {
        /* The adapter's init() calls SPIFFS.begin(true, "") — formatOnFail with
         * an EMPTY base path. That is a no-op only because setup() already
         * mounted SPIFFS and the framework returns early when mounted. Verify
         * that rather than assume it: if this skill ever runs before the mount,
         * the adapter would format the partition and take /mesh_identity.id,
         * /gw_token.txt, /wg.json and the notify store with it. */
        if (SPIFFS.totalBytes() == 0) {
            rns_error = "SPIFFS not mounted; refusing to let the adapter mount it";
        } else if (!rns_filesystem.init(false)) {
            rns_error = "SPIFFS adapter failed to attach";
        } else {
            RNS::Utilities::OS::register_filesystem(rns_filesystem);
            /* Matches the -DRNS_LOG_LEVEL=RNS_LOG_LEVEL_ERROR compile-time
             * floor: anything above ERROR is not in the binary to emit, and
             * LOG_TRACE would in any case stall the loop task on the console. */
            RNS::loglevel(RNS::LOG_ERROR);
            RNS::Reticulum::transport_enabled(false);
            RNS::Reticulum::probe_destination_enabled(false);
            rns_stack = RNS::Reticulum();
            rns_stack.start();
            rns_started = true;
        }
    } catch (const std::exception &e) {
        rns_started = false;
        rns_error = "exception during bring-up";
        Serial.printf("[rns] bring-up failed: %s\n", e.what());
    } catch (...) {
        rns_started = false;
        rns_error = "unknown exception during bring-up";
    }

    /* Separate scope: the identity is layered on top of a stack that already
     * started, so a throw here must not un-report rns_started. */
    if (rns_started) {
        try {
            rns_identity_bring_up();
        } catch (const std::exception &e) {
            rns_error = "exception during identity bring-up";
            Serial.printf("[rns] identity failed: %s\n", e.what());
        } catch (...) {
            rns_error = "unknown exception during identity bring-up";
        }
    }

    Serial.printf("[rns] started=%d identity=%d hash=%s%s%s\n",
                  (int)rns_started, (int)rns_identity_ok,
                  rns_hexhash[0] ? rns_hexhash : "-",
                  rns_error ? " err=" : "", rns_error ? rns_error : "");
    skill_register(&rns_skill);
    event_add("rns skill started=%d id=%d", (int)rns_started,
              (int)rns_identity_ok);
}
