#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
main = (root / "src/main.cpp").read_text(encoding="utf-8")
store = (root / "src/wifi_profile_store.cpp").read_text(encoding="utf-8")

persist = main[main.index("static bool wifi_persist_profiles()"):
               main.index("static bool wifi_load_legacy_config()")]
load = main[main.index("static void wifi_load_config()"):
            main.index("static bool wifi_save_config")]
legacy = main[main.index("static bool wifi_load_legacy_config()"):
              main.index("static void wifi_load_config()")]

assert "wifi_profile_nvs_save(&stored)" in persist
assert "return nvs_ok || file_ok" in persist
assert "wifi_profile_nvs_load(&stored)" in load
assert load.index("wifi_profile_nvs_load") < load.index("wifi_load_legacy_config")
assert "if (wifi_load_legacy_config()) wifi_persist_profiles();" in load
assert "read_spiffs_file(WIFI_CONFIG_FILE)" in legacy
assert 'WIFI_PROFILE_NVS_NS "wifi_profiles"' in store
assert 'WIFI_PROFILE_KEY_A  "blob0"' in store and 'WIFI_PROFILE_KEY_B  "blob1"' in store
assert "putBytes(key, raw, len) == len" in store
assert "memcmp(raw, verify, len) == 0" in store
assert "wifi_profile_equal(&current, set)" in store

print("Wi-Fi NVS integration tests: OK")
