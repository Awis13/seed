#pragma once
/*
 * mesh_secret_bridge.h -- a two-function window onto the NVS secret store for
 * the mc_client translation unit.
 *
 * mc_client.cpp is the MeshCore crypto TU (Mesh.h, BaseChatMesh, Identity). It
 * deliberately keeps its include surface narrow, exactly like mesh_contacts_fs()
 * hands it the ONE filesystem the contact table lives on instead of letting it
 * assume SPIFFS. This does the same for the mesh identity: two thin pass-throughs
 * to secret_store_get/put (defined in secret_store.cpp), so mc_client can load
 * the identity NVS-first and persist a freshly minted one without pulling in the
 * full secret_store.h + Preferences stack. One accessor means the boot migration,
 * the meshcore status read, and this crypto path cannot drift onto three stores.
 */

#include <stddef.h>
#include <stdint.h>

/* Copy the NVS secret `name` into buf (capacity cap); returns its byte length,
 * or 0 if absent / unreadable / larger than cap. */
size_t mesh_secret_get(const char *name, uint8_t *buf, size_t cap);

/* Store len bytes under `name`; returns true on success. */
bool mesh_secret_put(const char *name, const uint8_t *buf, size_t len);
