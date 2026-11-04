/* SPDX-License-Identifier: MIT
 * Exercise the complete production dispatcher and label store with real
 * owner/nanopb codecs. Only Zephyr, ZMK APIs and the Studio allocation
 * boundary are controlled. All address bytes and labels below are synthetic
 * fixtures; they do not identify bonded devices.
 */
#include <assert.h>
#include <errno.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zmk/ble_management/ble_management.pb.h>

#define IS_ENABLED(x) (x)
#define LOG_MODULE_DECLARE(...)
#define LOG_MODULE_REGISTER(...)
#define LOG_DBG(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#include <zephyr/bluetooth/addr.h>
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ZMK_STUDIO_RPC_HANDLER_UNSECURED 0
#define ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(...) .unused = 0
#define ZMK_RPC_CUSTOM_SUBSYSTEM(...)
#define ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(...)
#define ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(name, callback)      \
  allocate_response(callback)
#define SETTINGS_STATIC_HANDLER_DEFINE(...)

typedef int (*settings_read_cb)(void *, void *, size_t);
struct zmk_rpc_custom_subsystem_meta {
  int unused, security;
};
typedef struct {
  struct {
    size_t size;
    uint8_t bytes[128];
  } payload;
} zmk_custom_CallRequest;
enum zmk_transport { ZMK_TRANSPORT_USB, ZMK_TRANSPORT_BLE };
static unsigned saves, deletes, selects, clears, clear_alls, transports;
static int delete_failure;
static const char *delete_failure_key;
enum zmk_studio_core_lock_state {
  ZMK_STUDIO_CORE_LOCK_STATE_LOCKED = 0,
  ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED = 1,
};
static enum zmk_studio_core_lock_state lock_state =
    ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
static enum zmk_studio_core_lock_state zmk_studio_core_get_lock_state(void) {
  return lock_state;
}
static zmk_ble_management_Response response;
static char saved_key[64], saved_value[32], deleted_keys[8][64];
static size_t saved_size;
static bt_addr_le_t addresses[ZMK_BLE_PROFILE_COUNT];
static uint8_t active_profile_index;
static void (*selection_wait_hook)(uint8_t);
static void (*settings_delete_wait_hook)(void);

/* Bluetooth links seen by bt_conn_foreach(). */
struct bt_conn {
  size_t id;
};
enum bt_conn_type { BT_CONN_TYPE_LE = 1 };
enum { BT_CONN_ROLE_CENTRAL = 0, BT_CONN_ROLE_PERIPHERAL = 1 };
enum bt_conn_state {
  BT_CONN_STATE_DISCONNECTED,
  BT_CONN_STATE_CONNECTING,
  BT_CONN_STATE_CONNECTED,
  BT_CONN_STATE_DISCONNECTING
};
struct bt_conn_info {
  uint8_t type, role;
  enum bt_conn_state state;
};
static struct {
  uint8_t role;
  enum bt_conn_state state;
} links[4];
static size_t link_count;
static void bt_conn_foreach(enum bt_conn_type type,
                            void (*func)(struct bt_conn *, void *),
                            void *data) {
  assert(type == BT_CONN_TYPE_LE);
  for (size_t i = 0; i < link_count; i++) {
    struct bt_conn conn = {i};
    func(&conn, data);
  }
}
static int bt_conn_get_info(const struct bt_conn *conn,
                            struct bt_conn_info *info) {
  info->type = BT_CONN_TYPE_LE;
  info->role = links[conn->id].role;
  info->state = links[conn->id].state;
  return 0;
}

static zmk_ble_management_Response *allocate_response(pb_callback_t *callback) {
  memset(&response, 0, sizeof(response));
  memset(callback, 0, sizeof(*callback));
  return &response;
}
size_t hex2bin(const char *hex, size_t length, unsigned char *out,
               size_t capacity) {
  if (length != 2 || capacity < 1)
    return 0;
  unsigned int value = 0;
  for (size_t i = 0; i < 2; i++) {
    unsigned char c = (unsigned char)hex[i];
    unsigned int digit;
    if (c >= '0' && c <= '9')
      digit = c - '0';
    else if (c >= 'a' && c <= 'f')
      digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F')
      digit = c - 'A' + 10;
    else
      return 0;
    value = value * 16 + digit;
  }
  *out = (unsigned char)value;
  return 1;
}
int bt_rand(void *out, size_t size) {
  (void)out;
  (void)size;
  return -ENOTSUP;
}
static int settings_save_one(const char *name, const void *value, size_t size) {
  assert(strlen(name) < sizeof(saved_key));
  assert(size <= sizeof(saved_value));
  saves++;
  strcpy(saved_key, name);
  memcpy(saved_value, value, size);
  saved_size = size;
  return 0;
}
static int settings_delete(const char *name) {
  assert(strlen(name) < sizeof(deleted_keys[0]));
  if (deletes < ARRAY_SIZE(deleted_keys))
    strcpy(deleted_keys[deletes], name);
  deletes++;
  if (settings_delete_wait_hook) {
    void (*hook)(void) = settings_delete_wait_hook;
    settings_delete_wait_hook = NULL;
    hook();
  }
  return !delete_failure_key || strcmp(delete_failure_key, name) == 0
             ? delete_failure
             : 0;
}
static bool deleted(const char *name) {
  for (size_t i = 0; i < deletes && i < ARRAY_SIZE(deleted_keys); i++)
    if (strcmp(deleted_keys[i], name) == 0)
      return true;
  return false;
}
static bool settings_name_steq(const char *name, const char *key,
                               const char **next) {
  size_t n = strlen(key);
  *next = NULL;
  if (strncmp(name, key, n) != 0 || (name[n] != '/' && name[n] != '\0'))
    return false;
  if (name[n] == '/')
    *next = name + n + 1;
  return true;
}
static int zmk_ble_active_profile_index(void) { return active_profile_index; }
static bool zmk_ble_profile_is_open(uint8_t i) {
  assert(i < ZMK_BLE_PROFILE_COUNT);
  return bt_addr_le_eq(&addresses[i], BT_ADDR_LE_ANY);
}
static bool zmk_ble_profile_is_connected(uint8_t i) {
  (void)i;
  return false;
}
static bt_addr_le_t *zmk_ble_profile_address(uint8_t i) {
  assert(i < ZMK_BLE_PROFILE_COUNT);
  return &addresses[i];
}
static int zmk_ble_profile_index(const bt_addr_le_t *addr) {
  for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++)
    if (bt_addr_le_eq(&addresses[i], addr))
      return i;
  return -ENODEV;
}
static int zmk_ble_prof_select(uint8_t i) {
  selects++;
  active_profile_index = i;
  if (selection_wait_hook) {
    void (*hook)(uint8_t) = selection_wait_hook;
    selection_wait_hook = NULL;
    hook(i);
  }
  return 0;
}
static void zmk_ble_clear_bonds(void) { clears++; }
static void zmk_ble_clear_all_bonds(void) { clear_alls++; }
static int zmk_endpoint_set_preferred_transport(enum zmk_transport transport) {
  (void)transport;
  transports++;
  return 0;
}
static enum zmk_transport zmk_endpoint_get_preferred_transport(void) {
  return ZMK_TRANSPORT_USB;
}

#include LABELS_SOURCE
#include HANDLER_SOURCE

static zmk_ble_management_PeerIdentity peer_identity(const bt_addr_le_t *peer) {
  zmk_ble_management_PeerIdentity identity =
      zmk_ble_management_PeerIdentity_init_zero;
  identity.address_type = peer->type;
  memcpy(identity.address, peer->a.val, sizeof(peer->a.val));
  return identity;
}

static void reset_labels(void) {
  memset(profile_names, 0, sizeof(profile_names));
  saves = deletes = selects = clears = clear_alls = transports = 0;
  delete_failure = 0;
  delete_failure_key = NULL;
  active_profile_index = 0;
  selection_wait_hook = NULL;
  settings_delete_wait_hook = NULL;
}
static void unbond_all(void) {
  for (size_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++)
    bt_addr_le_copy(&addresses[i], BT_ADDR_LE_ANY);
}

static void check_split_info(bool peripheral_up) {
  zmk_ble_management_Request req = zmk_ble_management_Request_init_zero;
  req.which_request_type = zmk_ble_management_Request_get_split_info_tag;
  zmk_custom_CallRequest raw = {0};
  pb_ostream_t request =
      pb_ostream_from_buffer(raw.payload.bytes, sizeof(raw.payload.bytes));
  assert(pb_encode(&request, zmk_ble_management_Request_fields, &req));
  raw.payload.size = request.bytes_written;
  /* A connected host link and a connecting half never count as the half. */
  links[0].role = BT_CONN_ROLE_PERIPHERAL;
  links[0].state = BT_CONN_STATE_CONNECTED;
  links[1].role = BT_CONN_ROLE_CENTRAL;
  links[1].state = BT_CONN_STATE_CONNECTING;
  links[2].role = BT_CONN_ROLE_CENTRAL;
  links[2].state =
      peripheral_up ? BT_CONN_STATE_CONNECTED : BT_CONN_STATE_DISCONNECTING;
  link_count = 3;
  pb_callback_t callback = {0};
  assert(ble_management_rpc_handle_request(&raw, &callback));

  uint8_t bytes[zmk_ble_management_Response_size];
  pb_ostream_t encoded = pb_ostream_from_buffer(bytes, sizeof(bytes));
  assert(pb_encode(&encoded, zmk_ble_management_Response_fields, &response));
  zmk_ble_management_Response decoded = zmk_ble_management_Response_init_zero;
  pb_istream_t input = pb_istream_from_buffer(bytes, encoded.bytes_written);
  assert(pb_decode(&input, zmk_ble_management_Response_fields, &decoded));
  assert(input.bytes_left == 0);
  assert(decoded.which_response_type ==
         zmk_ble_management_Response_get_split_info_tag);
  assert(decoded.response_type.get_split_info.has_info);
  const zmk_ble_management_SplitInfo *info =
      &decoded.response_type.get_split_info.info;
  assert(info->is_split == (CONFIG_ZMK_SPLIT_BLE != 0));
  assert(info->is_central == (CONFIG_ZMK_SPLIT_BLE != 0));
  assert(info->peripheral_connected == (CONFIG_ZMK_SPLIT_BLE && peripheral_up));
  assert(!info->central_bonded); /* a peripheral's view; Studio runs on the
                                    central */
  printf("split-info %s peripheral=%d wire=", TEST_ROLE, peripheral_up);
  for (size_t i = 0; i < encoded.bytes_written; i++) {
    printf("%02x", bytes[i]);
  }
  puts(" PASS");
}

struct reader {
  const char *bytes;
  size_t size;
  int result;
};
static int read_setting(void *arg, void *dst, size_t size) {
  struct reader *r = arg;
  size_t n = r->size < size ? r->size : size;
  memcpy(dst, r->bytes, n);
  return r->result;
}
static int load(const char *key, const char *value, size_t len, int result) {
  struct reader r = {value, len, result};
  return profile_names_settings_set(key, len, read_setting, &r);
}
static void check_storage_key(void) {
  puts("storage-key: same MAC, four independent LE address types, save/reload");
  fflush(stdout);
  reset_labels();
  for (unsigned int type = 0; type < 4; type++) {
    bt_addr_le_t a = {.type = type, .a = {{6, 5, 4, 3, 2, 1}}};
    char value[32], key[64];
    snprintf(value, sizeof(value), "type-%u", type);
    assert(save_profile_name(&a, value) == 0);
    snprintf(key, sizeof(key), "ble_mgmt/name/%02x/01:02:03:04:05:06", type);
    assert(strcmp(saved_key, key) == 0);
  }
  reset_labels();
  for (unsigned int type = 0; type < 4; type++) {
    bt_addr_le_t a = {.type = type, .a = {{6, 5, 4, 3, 2, 1}}};
    char value[32], key[64];
    snprintf(value, sizeof(value), "type-%u", type);
    snprintf(key, sizeof(key), "name/%02x/01:02:03:04:05:06", type);
    assert(load(key, value, strlen(value) + 1, (int)strlen(value) + 1) == 0);
    assert(strcmp(get_profile_name(&a), value) == 0);
  }
  /* A modern public record wins over a legacy public key in either order. */
  for (int order = 0; order < 2; order++) {
    reset_labels();
    const char *old = "name/01:02:03:04:05:06",
               *modern = "name/00/01:02:03:04:05:06";
    assert(load(order ? modern : old, order ? "" : "legacy", order ? 1 : 7,
                order ? 1 : 7) == 0);
    assert(load(order ? old : modern, order ? "legacy" : "", order ? 7 : 1,
                order ? 7 : 1) == 0);
    bt_addr_le_t a = {.type = BT_ADDR_LE_PUBLIC, .a = {{6, 5, 4, 3, 2, 1}}};
    assert(strcmp(get_profile_name(&a), "") == 0);
  }
  /* Unpairing deletes both stored keys and frees the slot for reuse. */
  bt_addr_le_t a = {.type = BT_ADDR_LE_PUBLIC, .a = {{6, 5, 4, 3, 2, 1}}};
  addresses[0] = a;
  zmk_ble_management_UnpairProfileRequest unpair =
      zmk_ble_management_UnpairProfileRequest_init_zero;
  unpair.has_expected_peer = true;
  unpair.expected_peer = peer_identity(&a);
  zmk_ble_management_Response result = zmk_ble_management_Response_init_zero;
  deletes = 0;
  assert(handle_unpair_profile(&unpair, &result) == 0);
  assert(result.response_type.unpair_profile.success);
  assert(deleted("ble_mgmt/name/00/01:02:03:04:05:06") &&
         deleted("ble_mgmt/name/01:02:03:04:05:06"));
  assert(strcmp(get_profile_name(&a), "") == 0);
  assert(load("name/01:02:03:04:05:06", "legacy", 7, 7) == 0);
  assert(strcmp(get_profile_name(&a), "legacy") == 0);
  unbond_all();
  puts("storage-key PASS");
}
static void check_storage_read(void) {
  puts("storage-read: negative/short/invalid reads preserve cache and empty "
       "slots");
  fflush(stdout);
  bt_addr_le_t a = {.type = BT_ADDR_LE_PUBLIC, .a = {{6, 5, 4, 3, 2, 1}}};
  const char *legacy = "name/01:02:03:04:05:06";
  reset_labels();
  assert(save_profile_name(&a, "stable") == 0);
  int rc = load(legacy, "poison", 7, -EIO);
  assert(strcmp(get_profile_name(&a), "stable") == 0);
  assert(rc == -EIO);
  rc = load(legacy, "short", 6, 2);
  assert(strcmp(get_profile_name(&a), "stable") == 0);
  assert(rc == -EIO);
  char full[32];
  memset(full, 'X', sizeof(full));
  assert(load(legacy, full, sizeof(full), sizeof(full)) == -EINVAL);
  assert(strcmp(get_profile_name(&a), "stable") == 0);
  char long_value[33];
  memset(long_value, 'Y', sizeof(long_value));
  assert(load(legacy, long_value, sizeof(long_value), sizeof(long_value)) ==
         -EINVAL);
  assert(load(legacy, "", 0, 0) == -EINVAL);
  assert(strcmp(get_profile_name(&a), "stable") == 0);
  /* Successful legacy records remain readable; failed reads allocate nothing.
   */
  reset_labels();
  assert(load(legacy, "poison", 7, -EIO) == -EIO);
  for (size_t i = 0; i < ARRAY_SIZE(profile_names); i++)
    assert(!profile_names[i].used);
  assert(load(legacy, "legacy", 7, 7) == 0);
  assert(strcmp(get_profile_name(&a), "legacy") == 0);
  char max_name[32];
  memset(max_name, 'M', 31);
  max_name[31] = '\0';
  assert(load(legacy, max_name, 32, 32) == 0);
  assert(strcmp(get_profile_name(&a), max_name) == 0);
  assert(load("name/01:02:03:04:05:06/extra", "bad", 4, 4) == 0);
  assert(load("name/04/01:02:03:04:05:06", "bad", 4, 4) == 0);
  assert(strcmp(get_profile_name(&a), max_name) == 0);
  puts("storage-read PASS");
}
static void check_label_lifecycle(void) {
  puts("labels: empty name deletes, stale labels are reclaimed, legacy keys "
       "retire");
  fflush(stdout);
  bt_addr_le_t a = {.type = BT_ADDR_LE_PUBLIC, .a = {{6, 5, 4, 3, 2, 1}}};
  /* An empty name deletes the label instead of storing "". */
  reset_labels();
  assert(save_profile_name(&a, "desk") == 0 && saves == 1);
  assert(save_profile_name(&a, "") == 0);
  assert(saves == 1 && deleted("ble_mgmt/name/00/01:02:03:04:05:06"));
  assert(strcmp(get_profile_name(&a), "") == 0 && !profile_names[0].used);
  /* Saving a typed key retires the legacy key it replaces, once. */
  reset_labels();
  assert(load("name/01:02:03:04:05:06", "legacy", 7, 7) == 0);
  assert(save_profile_name(&a, "typed") == 0 && deletes == 1 &&
         deleted("ble_mgmt/name/01:02:03:04:05:06"));
  assert(save_profile_name(&a, "again") == 0 && deletes == 1);
  /* Every slot holds a label of an unbonded host: a new label reclaims one
   * and deletes its record; with every label still bonded there is no room. */
  reset_labels();
  unbond_all();
  for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
    bt_addr_le_t stale = {.type = BT_ADDR_LE_RANDOM,
                          .a = {{(uint8_t)i, 9, 9, 9, 9, 9}}};
    assert(save_profile_name(&stale, "old") == 0);
  }
  deletes = 0;
  addresses[0] = a;
  assert(save_profile_name(&a, "new") == 0 &&
         strcmp(get_profile_name(&a), "new") == 0);
  assert(deletes == 1 && deleted("ble_mgmt/name/01/09:09:09:09:09:00"));
  for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++)
    addresses[i] = profile_names[i].addr;
  bt_addr_le_t other = {.type = BT_ADDR_LE_PUBLIC, .a = {{7, 7, 7, 7, 7, 7}}};
  assert(save_profile_name(&other, "full") == -ENOMEM);
  unbond_all();
  puts("labels PASS");
}
static zmk_ble_management_Response call(zmk_ble_management_Request req) {
  zmk_custom_CallRequest raw = {0};
  pb_ostream_t w =
      pb_ostream_from_buffer(raw.payload.bytes, sizeof(raw.payload.bytes));
  assert(pb_encode(&w, zmk_ble_management_Request_fields, &req));
  raw.payload.size = w.bytes_written;
  pb_callback_t cb = {0};
  assert(ble_management_rpc_handle_request(&raw, &cb));
  uint8_t bytes[zmk_ble_management_Response_size];
  w = pb_ostream_from_buffer(bytes, sizeof(bytes));
  assert(pb_encode(&w, zmk_ble_management_Response_fields, &response));
  zmk_ble_management_Response decoded = zmk_ble_management_Response_init_zero;
  pb_istream_t r = pb_istream_from_buffer(bytes, w.bytes_written);
  assert(pb_decode(&r, zmk_ble_management_Response_fields, &decoded));
  return decoded;
}
static void check_reclaim_failure(void) {
  puts("reclaim-failure: refused stale deletion preserves cache and refuses a "
       "new save");
  for (int legacy = 0; legacy < 2; legacy++) {
    reset_labels();
    unbond_all();
    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
      bt_addr_le_t old = {.type = BT_ADDR_LE_RANDOM,
                          .a = {{(uint8_t)i, 9, 9, 9, 9, 9}}};
      assert(save_profile_name(&old, "old") == 0);
      addresses[i] = old;
    }
    /* A public stale entry also exercises a partial typed/legacy deletion. */
    if (legacy)
      profile_names[0].addr.type = BT_ADDR_LE_PUBLIC;
    bt_addr_le_t stale = profile_names[0].addr;
    bt_addr_le_t current = {.type = BT_ADDR_LE_RANDOM,
                            .a = {{7, 7, 7, 7, 7, 7}}};
    addresses[0] = current;
    struct profile_name_entry before[ZMK_BLE_PROFILE_COUNT];
    memcpy(before, profile_names, sizeof(before));
    unsigned before_saves = saves;
    delete_failure = -EIO;
    if (legacy)
      delete_failure_key = "ble_mgmt/name/09:09:09:09:09:00";
    assert(save_profile_name(&current, "new") == -EIO);
    assert(saves == before_saves);
    assert(memcmp(before, profile_names, sizeof(before)) == 0);
    assert(strcmp(get_profile_name(&current), "") == 0);
    assert(strcmp(get_profile_name(&stale), "old") == 0);
    for (int i = 1; i < ZMK_BLE_PROFILE_COUNT; i++)
      assert(strcmp(get_profile_name(&addresses[i]), "old") == 0);
    delete_failure = 0;
    delete_failure_key = NULL;
    assert(save_profile_name(&current, "new") == 0);
    assert(saves == before_saves + 1);
    assert(strcmp(get_profile_name(&current), "new") == 0);
    assert(strcmp(get_profile_name(&stale), "") == 0);
    for (int i = 1; i < ZMK_BLE_PROFILE_COUNT; i++)
      assert(strcmp(get_profile_name(&addresses[i]), "old") == 0);
  }
  unbond_all();
  puts("reclaim-failure PASS");
}
static void check_open_profile(void) {
  puts("open-profile: core ANY slots and invalid NONE peers have no label "
       "target");
  reset_labels();
  unbond_all();
  assert(!bt_addr_le_eq(BT_ADDR_LE_ANY, BT_ADDR_LE_NONE));
  zmk_ble_management_Request req = zmk_ble_management_Request_init_zero;
  req.which_request_type = zmk_ble_management_Request_get_profiles_tag;
  zmk_ble_management_Response resp = call(req);
  assert(resp.which_response_type ==
         zmk_ble_management_Response_get_profiles_tag);
  assert(resp.response_type.get_profiles.profiles[0].is_open);
  assert(resp.response_type.get_profiles.profiles[0].address[0] == '\0');
  for (int sentinel = 0; sentinel < 2; sentinel++) {
    if (sentinel)
      bt_addr_le_copy(&addresses[0], BT_ADDR_LE_NONE);
    req = (zmk_ble_management_Request)zmk_ble_management_Request_init_zero;
    req.which_request_type = zmk_ble_management_Request_set_profile_name_tag;
    strcpy(req.request_type.set_profile_name.name, "fixture-name");
    resp = call(req);
    assert(resp.which_response_type == zmk_ble_management_Response_error_tag);
    assert(saves == 0);
  }
  bt_addr_le_t bonded = {.type = BT_ADDR_LE_RANDOM, .a = {{6, 5, 4, 3, 2, 1}}};
  addresses[0] = bonded;
  req.request_type.set_profile_name.has_expected_peer = true;
  req.request_type.set_profile_name.expected_peer = peer_identity(&bonded);
  resp = call(req);
  assert(resp.which_response_type ==
         zmk_ble_management_Response_set_profile_name_tag);
  assert(resp.response_type.set_profile_name.success && saves == 1);
  assert(strcmp(get_profile_name(&bonded), "fixture-name") == 0);
  req = (zmk_ble_management_Request)zmk_ble_management_Request_init_zero;
  req.which_request_type = zmk_ble_management_Request_get_profiles_tag;
  resp = call(req);
  assert(!resp.response_type.get_profiles.profiles[0].is_open);
  assert(strcmp(resp.response_type.get_profiles.profiles[0].address,
                "01:02:03:04:05:06") == 0);
  unbond_all();
  puts("open-profile PASS");
}
static void check_profiles(void) {
  puts("profiles: bounded real protobuf response");
  fflush(stdout);
  zmk_ble_management_Request req = zmk_ble_management_Request_init_zero;
  req.which_request_type = zmk_ble_management_Request_get_profiles_tag;
  zmk_ble_management_Response decoded = call(req);
  if (ZMK_BLE_PROFILE_COUNT > 5) {
    assert(decoded.which_response_type ==
           zmk_ble_management_Response_error_tag);
    assert(strstr(decoded.response_type.error.message, "-7") !=
           NULL); /* -E2BIG */
  } else {
    assert(decoded.which_response_type ==
           zmk_ble_management_Response_get_profiles_tag);
    assert(decoded.response_type.get_profiles.profiles_count ==
           ZMK_BLE_PROFILE_COUNT);
    for (size_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++)
      assert(decoded.response_type.get_profiles.profiles[i].index == i);
  }
  printf("profiles %s count=%d PASS\n", TEST_ROLE, ZMK_BLE_PROFILE_COUNT);
}
static void check_forget_split(void) {
  puts("forget-split: host bonds are cleared and their labels deleted");
  fflush(stdout);
  reset_labels();
  unbond_all();
  bt_addr_le_t a = {.type = BT_ADDR_LE_PUBLIC, .a = {{6, 5, 4, 3, 2, 1}}};
  addresses[1] = a;
  assert(save_profile_name(&a, "desk") == 0);
  zmk_ble_management_Request req = zmk_ble_management_Request_init_zero;
  req.which_request_type = zmk_ble_management_Request_forget_split_bond_tag;
  zmk_ble_management_Response resp = call(req);
  assert(resp.which_response_type ==
         zmk_ble_management_Response_forget_split_bond_tag);
  assert(resp.response_type.forget_split_bond.success ==
         (CONFIG_ZMK_SPLIT_BLE != 0));
  assert(clear_alls == (CONFIG_ZMK_SPLIT_BLE ? 1u : 0u));
  assert(strcmp(get_profile_name(&a), CONFIG_ZMK_SPLIT_BLE ? "" : "desk") == 0);
  unbond_all();
  printf("forget-split %s PASS\n", TEST_ROLE);
}
static void check_unlock(void) {
  puts(
      "unlock: changes need an unlocked Studio; addresses only while unlocked");
  fflush(stdout);
  reset_labels();
  bt_addr_le_t a = {.type = BT_ADDR_LE_PUBLIC, .a = {{6, 5, 4, 3, 2, 1}}};
  addresses[0] = a;
  assert(save_profile_name(&a, "desk") == 0);
  const pb_size_t changes[] = {
      zmk_ble_management_Request_set_profile_name_tag,
      zmk_ble_management_Request_switch_profile_tag,
      zmk_ble_management_Request_unpair_profile_tag,
      zmk_ble_management_Request_forget_split_bond_tag,
      zmk_ble_management_Request_set_output_priority_tag};
  lock_state = ZMK_STUDIO_CORE_LOCK_STATE_LOCKED;
  saves = deletes = selects = clears = clear_alls = transports = 0;
  for (size_t i = 0; i < ARRAY_SIZE(changes); i++) {
    zmk_ble_management_Request req = zmk_ble_management_Request_init_zero;
    req.which_request_type = changes[i];
    if (changes[i] == zmk_ble_management_Request_set_profile_name_tag) {
      strcpy(req.request_type.set_profile_name.name, "renamed");
    }
    zmk_ble_management_Response resp = call(req);
    assert(resp.which_response_type == zmk_ble_management_Response_error_tag);
    assert(strcmp(resp.response_type.error.message, "Unlock required") == 0);
  }
  assert(saves == 0 && deletes == 0 && selects == 0 && clears == 0 &&
         clear_alls == 0 && transports == 0);
  assert(strcmp(get_profile_name(&a), "desk") == 0);
  /* Reads still work while locked, without the bonded host's address. */
  zmk_ble_management_Request list = zmk_ble_management_Request_init_zero;
  list.which_request_type = zmk_ble_management_Request_get_profiles_tag;
  zmk_ble_management_Response resp = call(list);
  assert(resp.which_response_type ==
         zmk_ble_management_Response_get_profiles_tag);
  assert(resp.response_type.get_profiles.profiles[0].address[0] == '\0');
  assert(!resp.response_type.get_profiles.profiles[0].has_peer_identity);
  assert(strcmp(resp.response_type.get_profiles.profiles[0].name, "desk") == 0);
  zmk_ble_management_Request split = zmk_ble_management_Request_init_zero;
  split.which_request_type = zmk_ble_management_Request_get_split_info_tag;
  assert(call(split).which_response_type ==
         zmk_ble_management_Response_get_split_info_tag);
  zmk_ble_management_Request output = zmk_ble_management_Request_init_zero;
  output.which_request_type =
      zmk_ble_management_Request_get_output_priority_tag;
  assert(call(output).which_response_type ==
         zmk_ble_management_Response_get_output_priority_tag);
  lock_state = ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
  resp = call(list);
  assert(strcmp(resp.response_type.get_profiles.profiles[0].address,
                "01:02:03:04:05:06") == 0);
  zmk_ble_management_Request rename = zmk_ble_management_Request_init_zero;
  rename.which_request_type = zmk_ble_management_Request_set_profile_name_tag;
  rename.request_type.set_profile_name.has_expected_peer = true;
  rename.request_type.set_profile_name.expected_peer = peer_identity(&a);
  strcpy(rename.request_type.set_profile_name.name, "renamed");
  resp = call(rename);
  assert(resp.which_response_type ==
         zmk_ble_management_Response_set_profile_name_tag);
  assert(saves == 1 && strcmp(get_profile_name(&a), "renamed") == 0);
  unbond_all();
  puts("unlock PASS");
}

static void check_peer_identity(void) {
  puts("peer-identity: old, replaced, wrong-type and missing peers cannot "
       "mutate a slot");
  reset_labels();
  unbond_all();
  lock_state = ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
  bt_addr_le_t old = {.type = BT_ADDR_LE_PUBLIC, .a = {{6, 5, 4, 3, 2, 1}}};
  addresses[1] = old;
  zmk_ble_management_Request list = zmk_ble_management_Request_init_zero;
  list.which_request_type = zmk_ble_management_Request_get_profiles_tag;
  zmk_ble_management_Response listed = call(list);
  assert(listed.response_type.get_profiles.profiles[1].has_peer_identity);
  zmk_ble_management_PeerIdentity observed =
      listed.response_type.get_profiles.profiles[1].peer_identity;
  assert(observed.address_type == old.type);
  assert(memcmp(observed.address, old.a.val, sizeof(old.a.val)) == 0);

  zmk_ble_management_Request rename = zmk_ble_management_Request_init_zero;
  rename.which_request_type = zmk_ble_management_Request_set_profile_name_tag;
  rename.request_type.set_profile_name.index = 1;
  rename.request_type.set_profile_name.has_expected_peer = true;
  rename.request_type.set_profile_name.expected_peer = observed;
  strcpy(rename.request_type.set_profile_name.name, "current");
  zmk_ble_management_Request unpair = zmk_ble_management_Request_init_zero;
  unpair.which_request_type = zmk_ble_management_Request_unpair_profile_tag;
  unpair.request_type.unpair_profile.index = 1;
  unpair.request_type.unpair_profile.has_expected_peer = true;
  unpair.request_type.unpair_profile.expected_peer = observed;

  /* Identical MAC bytes with a replacement address type are a different peer.
   */
  addresses[1].type = BT_ADDR_LE_RANDOM;
  assert(call(rename).which_response_type ==
         zmk_ble_management_Response_error_tag);
  assert(call(unpair).which_response_type ==
         zmk_ble_management_Response_error_tag);
  assert(saves == 0 && deletes == 0 && selects == 0 && clears == 0);
  addresses[1] = old;
  addresses[1].a.val[0] ^= 0x80;
  assert(call(rename).which_response_type ==
         zmk_ble_management_Response_error_tag);
  assert(call(unpair).which_response_type ==
         zmk_ble_management_Response_error_tag);
  assert(saves == 0 && deletes == 0 && selects == 0 && clears == 0);

  /* Refuse legacy requests even when their index currently has a bonded peer.
   */
  rename.request_type.set_profile_name.has_expected_peer = false;
  unpair.request_type.unpair_profile.has_expected_peer = false;
  assert(call(rename).which_response_type ==
         zmk_ble_management_Response_error_tag);
  assert(call(unpair).which_response_type ==
         zmk_ble_management_Response_error_tag);
  assert(saves == 0 && deletes == 0 && selects == 0 && clears == 0);

  rename.request_type.set_profile_name.has_expected_peer = true;
  rename.request_type.set_profile_name.expected_peer =
      peer_identity(&addresses[1]);
  zmk_ble_management_Response changed = call(rename);
  assert(changed.which_response_type ==
         zmk_ble_management_Response_set_profile_name_tag);
  assert(changed.response_type.set_profile_name.success);
  assert(strcmp(get_profile_name(&addresses[1]), "current") == 0 && saves == 1);
  unpair.request_type.unpair_profile.has_expected_peer = true;
  unpair.request_type.unpair_profile.expected_peer =
      peer_identity(&addresses[1]);
  changed = call(unpair);
  assert(changed.which_response_type ==
         zmk_ble_management_Response_unpair_profile_tag);
  assert(changed.response_type.unpair_profile.success && clears == 1);
  assert(strcmp(get_profile_name(&addresses[1]), "") == 0);
  assert(active_profile_index == 0 && selects == 2);
  unbond_all();
  puts("peer-identity PASS");
}

static const bt_addr_le_t original_peer = {.type = BT_ADDR_LE_PUBLIC,
                                           .a = {{6, 5, 4, 3, 2, 1}}};
static const bt_addr_le_t replacement_peer = {
    .type = BT_ADDR_LE_RANDOM, .a = {{0x16, 0x15, 0x14, 0x13, 0x12, 0x11}}};
static unsigned wait_handoffs;

static void replace_peer_during_settings_wait(void) {
  wait_handoffs++;
  addresses[1] = replacement_peer;
}

static void prime_full_label_store(void) {
  reset_labels();
  unbond_all();
  wait_handoffs = 0;
  addresses[1] = original_peer;
  for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
    bt_addr_le_t stale = {.type = BT_ADDR_LE_RANDOM,
                          .a = {{(uint8_t)i, 9, 9, 9, 9, 9}}};
    assert(save_profile_name(&stale, "stale") == 0);
  }
  settings_delete_wait_hook = replace_peer_during_settings_wait;
}

/* Zephyr settings_delete() takes settings_lock with K_FOREVER. The hook
 * models a profile replacement at that actual blocking-call boundary; it
 * does not claim a measured Bluetooth or storage schedule. */
static void check_rename_reclaim_peer(void) {
  prime_full_label_store();
  lock_state = ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
  zmk_ble_management_Request rename = zmk_ble_management_Request_init_zero;
  rename.which_request_type = zmk_ble_management_Request_set_profile_name_tag;
  rename.request_type.set_profile_name.index = 1;
  rename.request_type.set_profile_name.has_expected_peer = true;
  rename.request_type.set_profile_name.expected_peer =
      peer_identity(&original_peer);
  strcpy(rename.request_type.set_profile_name.name, "owned");
  zmk_ble_management_Response result = call(rename);
  assert(result.which_response_type ==
             zmk_ble_management_Response_set_profile_name_tag &&
         result.response_type.set_profile_name.success);
  assert(wait_handoffs == 1 && bt_addr_le_eq(&addresses[1], &replacement_peer));
  assert(strcmp(saved_key, "ble_mgmt/name/00/01:02:03:04:05:06") == 0 &&
         "Rename changed its peer during settings reclaim");
  assert(strcmp(get_profile_name(&original_peer), "owned") == 0);
  assert(strcmp(get_profile_name(&replacement_peer), "") == 0);
  puts("rename-reclaim PASS");
}

static void check_label_save_peer(void) {
  prime_full_label_store();
  assert(save_profile_name(&addresses[1], "owned") == 0);
  assert(wait_handoffs == 1 && bt_addr_le_eq(&addresses[1], &replacement_peer));
  assert(strcmp(saved_key, "ble_mgmt/name/00/01:02:03:04:05:06") == 0 &&
         "Label save followed a borrowed peer across settings reclaim");
  assert(strcmp(get_profile_name(&original_peer), "owned") == 0);
  assert(strcmp(get_profile_name(&replacement_peer), "") == 0);
  puts("label-save-peer PASS");
}

static void check_label_delete_peer(void) {
  reset_labels();
  unbond_all();
  wait_handoffs = 0;
  addresses[1] = original_peer;
  assert(load("name/01:02:03:04:05:06", "legacy", 7, 7) == 0);
  assert(save_profile_name(&replacement_peer, "independent") == 0);
  settings_delete_wait_hook = replace_peer_during_settings_wait;
  assert(delete_profile_name(&addresses[1]) == 0);
  assert(wait_handoffs == 1 && bt_addr_le_eq(&addresses[1], &replacement_peer));
  assert(deleted("ble_mgmt/name/00/01:02:03:04:05:06") &&
         deleted("ble_mgmt/name/01:02:03:04:05:06") &&
         "Label delete lost its original peer during settings deletion");
  assert(!deleted("ble_mgmt/name/01/11:12:13:14:15:16"));
  assert(strcmp(get_profile_name(&replacement_peer), "independent") == 0);
  puts("label-delete-peer PASS");
}

/* DYA prof_select() changes active_profile, then may block in advertising's
 * synchronous HCI command. These one-shot hooks model independent changes
 * before that API returns, rather than an invented callback between pure
 * comparisons. The clear stub remains the original controlled side effect. */
static void choose_other_profile_during_selection(uint8_t index) {
  assert(index == 1);
  wait_handoffs++;
  active_profile_index = 2;
}

static void replace_peer_during_selection(uint8_t index) {
  assert(index == 1);
  wait_handoffs++;
  addresses[1] = replacement_peer;
}

static void check_unpair_selection_peer(bool replacement) {
  reset_labels();
  unbond_all();
  wait_handoffs = 0;
  lock_state = ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
  addresses[1] = original_peer;
  if (!replacement) {
    addresses[2] = replacement_peer;
  }
  assert(save_profile_name(&original_peer, "original") == 0);
  assert(save_profile_name(&replacement_peer, "independent") == 0);
  selection_wait_hook = replacement ? replace_peer_during_selection
                                    : choose_other_profile_during_selection;
  zmk_ble_management_Request unpair = zmk_ble_management_Request_init_zero;
  unpair.which_request_type = zmk_ble_management_Request_unpair_profile_tag;
  unpair.request_type.unpair_profile.index = 1;
  unpair.request_type.unpair_profile.has_expected_peer = true;
  unpair.request_type.unpair_profile.expected_peer =
      peer_identity(&original_peer);
  zmk_ble_management_Response result = call(unpair);
  assert(wait_handoffs == 1);
  assert(clears == 0 &&
         "Unpair accepted a changed selection or peer after its HCI wait");
  assert(result.which_response_type == zmk_ble_management_Response_error_tag);
  assert(deletes == 0 && selects == 1);
  assert(active_profile_index == (replacement ? 1 : 2));
  assert(strcmp(get_profile_name(&original_peer), "original") == 0);
  assert(strcmp(get_profile_name(&replacement_peer), "independent") == 0);
  puts(replacement ? "unpair-replacement PASS" : "unpair-selection PASS");
}
static void check_all_split_info(void) {
  check_split_info(false);
  check_split_info(true);
  /* Malformed and unsupported input must not reuse a prior info response. */
  zmk_custom_CallRequest malformed = {.payload = {.size = 1, .bytes = {0x80}}};
  pb_callback_t callback = {0};
  assert(ble_management_rpc_handle_request(&malformed, &callback));
  assert(response.which_response_type == zmk_ble_management_Response_error_tag);
  zmk_custom_CallRequest empty = {0};
  assert(ble_management_rpc_handle_request(&empty, &callback));
  assert(response.which_response_type == zmk_ble_management_Response_error_tag);
  check_split_info(false);
}
int main(int argc, char **argv) {
  const char *test = argc > 1 ? argv[1] : "all";
  unbond_all();
  if (strcmp(test, "all") == 0 || strcmp(test, "split-info") == 0)
    check_all_split_info();
  if (strcmp(test, "all") == 0 || strcmp(test, "storage-key") == 0)
    check_storage_key();
  if (strcmp(test, "all") == 0 || strcmp(test, "storage-read") == 0)
    check_storage_read();
  if (strcmp(test, "all") == 0 || strcmp(test, "labels") == 0)
    check_label_lifecycle();
  if (strcmp(test, "all") == 0 || strcmp(test, "profiles") == 0)
    check_profiles();
  if (strcmp(test, "all") == 0 || strcmp(test, "forget-split") == 0)
    check_forget_split();
  if (strcmp(test, "all") == 0 || strcmp(test, "unlock") == 0)
    check_unlock();
  if (strcmp(test, "all") == 0 || strcmp(test, "reclaim-failure") == 0)
    check_reclaim_failure();
  if (strcmp(test, "all") == 0 || strcmp(test, "open-profile") == 0)
    check_open_profile();
  if (strcmp(test, "all") == 0 || strcmp(test, "peer-identity") == 0)
    check_peer_identity();
  if (strcmp(test, "all") == 0 || strcmp(test, "rename-reclaim") == 0)
    check_rename_reclaim_peer();
  if (strcmp(test, "all") == 0 || strcmp(test, "label-save-peer") == 0)
    check_label_save_peer();
  if (strcmp(test, "all") == 0 || strcmp(test, "label-delete-peer") == 0)
    check_label_delete_peer();
  if (strcmp(test, "all") == 0 || strcmp(test, "unpair-selection") == 0)
    check_unpair_selection_peer(false);
  if (strcmp(test, "all") == 0 || strcmp(test, "unpair-replacement") == 0)
    check_unpair_selection_peer(true);
  return 0;
}
