/**
 * BLE management custom Studio RPC: lists the host profiles with their
 * labels, switches and unpairs profiles, reports the split role and sets the
 * preferred output (USB or BLE).
 *
 * The subsystem stays UNSECURED so the lists can be read while Studio is
 * locked. Changes to bonds, the active profile, labels and the output affect
 * every host, and a bonded host's address identifies that device, so both
 * need an unlocked Studio.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/sys/util.h>
#include <zmk/ble.h>
#include <zmk/ble_management/ble_management.pb.h>
#include <zmk/endpoints.h>
#include <zmk/studio/core.h>
#include <zmk/studio/custom.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include "../profile_labels.h"
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
#include <zephyr/bluetooth/conn.h>
#endif

#include <zephyr/logging/log.h>
#ifndef CONFIG_ZMK_MODULE_BLE_MANAGEMENT_LOG_LEVEL
#define CONFIG_ZMK_MODULE_BLE_MANAGEMENT_LOG_LEVEL 3
#endif
LOG_MODULE_REGISTER(zmk_module_ble_management_ble_management_handler,
                    CONFIG_ZMK_MODULE_BLE_MANAGEMENT_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta ble_management_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(
        "https://cormoran.github.io/zmk-module-ble-management/"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran_ble, &ble_management_meta,
                         ble_management_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran_ble,
                                         zmk_ble_management_Response);

static bool studio_unlocked(void) {
  return zmk_studio_core_get_lock_state() ==
         ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
}

/* Only the reads answer while locked; any other request, including one a
 * later schema adds, needs an unlocked Studio. */
static bool request_needs_unlock(pb_size_t request_type) {
  switch (request_type) {
  case zmk_ble_management_Request_get_profiles_tag:
  case zmk_ble_management_Request_get_split_info_tag:
  case zmk_ble_management_Request_get_output_priority_tag:
    return false;
  default:
    return true;
  }
}

static void set_error(zmk_ble_management_Response *resp, const char *message) {
  zmk_ble_management_ErrorResponse err =
      zmk_ble_management_ErrorResponse_init_zero;
  snprintf(err.message, sizeof(err.message), "%s", message);
  resp->which_response_type = zmk_ble_management_Response_error_tag;
  resp->response_type.error = err;
}

#if IS_ENABLED(CONFIG_ZMK_BLE)
/* The bonded host address of profile `index`, or NULL if the index is out of
 * range or the profile is open. */
static const bt_addr_le_t *profile_peer(uint32_t index) {
  if (index >= ZMK_BLE_PROFILE_COUNT) {
    LOG_WRN("Invalid profile index: %d", index);
    return NULL;
  }
  const bt_addr_le_t *addr = zmk_ble_profile_address(index);
  return addr && !bt_addr_le_eq(addr, BT_ADDR_LE_ANY) &&
                 !bt_addr_le_eq(addr, BT_ADDR_LE_NONE)
             ? addr
             : NULL;
}

static bool
profile_peer_matches(uint32_t index, bool has_expected_peer,
                     const zmk_ble_management_PeerIdentity *expected,
                     bt_addr_le_t *matched) {
  const bt_addr_le_t *current = profile_peer(index);
  if (!has_expected_peer || !current) {
    return false;
  }
  bt_addr_le_t peer;
  bt_addr_le_copy(&peer, current);
  if (expected->address_type != peer.type ||
      memcmp(expected->address, peer.a.val, sizeof(peer.a.val)) != 0) {
    return false;
  }
  if (matched) {
    bt_addr_le_copy(matched, &peer);
  }
  return true;
}
#endif

static int handle_get_profiles(zmk_ble_management_Response *resp) {
  LOG_DBG("GetProfilesRequest");
  resp->which_response_type = zmk_ble_management_Response_get_profiles_tag;

#if IS_ENABLED(CONFIG_ZMK_BLE)
  zmk_ble_management_GetProfilesResponse *result =
      &resp->response_type.get_profiles;
  /* The wire schema has room for five profiles; never overrun its array. */
  if (ZMK_BLE_PROFILE_COUNT > ARRAY_SIZE(result->profiles)) {
    return -E2BIG;
  }
  result->max_profiles = ZMK_BLE_PROFILE_COUNT;
  result->profiles_count = ZMK_BLE_PROFILE_COUNT;

  const int active = zmk_ble_active_profile_index();
  /* Labels stay visible while locked; host addresses do not. */
  const bool include_address = studio_unlocked();

  for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
    zmk_ble_management_ProfileInfo *profile = &result->profiles[i];
    profile->index = i;
    profile->is_open = zmk_ble_profile_is_open(i);
    profile->is_connected = zmk_ble_profile_is_connected(i);
    profile->is_active = (i == active);

    const bt_addr_le_t *addr = profile_peer(i);
    if (!addr) {
      continue;
    }
    if (include_address) {
      bt_addr_to_str(&addr->a, profile->address, sizeof(profile->address));
      profile->has_peer_identity = true;
      profile->peer_identity.address_type = addr->type;
      memcpy(profile->peer_identity.address, addr->a.val, sizeof(addr->a.val));
    }
    strncpy(profile->name, get_profile_name(addr), sizeof(profile->name) - 1);
  }
#endif
  return 0;
}

static int
handle_set_profile_name(const zmk_ble_management_SetProfileNameRequest *req,
                        zmk_ble_management_Response *resp) {
  LOG_DBG("profile label update requested: profile=%d", req->index);
  resp->which_response_type = zmk_ble_management_Response_set_profile_name_tag;

#if IS_ENABLED(CONFIG_ZMK_BLE)
  bt_addr_le_t peer;
  if (!profile_peer_matches(req->index, req->has_expected_peer,
                            &req->expected_peer, &peer)) {
    set_error(resp, "Profile changed or identity missing; refresh profiles");
    return 0;
  }
  resp->response_type.set_profile_name.success =
      save_profile_name(&peer, req->name) == 0;
#endif
  return 0;
}

static int
handle_switch_profile(const zmk_ble_management_SwitchProfileRequest *req,
                      zmk_ble_management_Response *resp) {
  LOG_DBG("SwitchProfileRequest: index=%d", req->index);
  resp->which_response_type = zmk_ble_management_Response_switch_profile_tag;

#if IS_ENABLED(CONFIG_ZMK_BLE)
  if (req->index < ZMK_BLE_PROFILE_COUNT) {
    resp->response_type.switch_profile.success =
        zmk_ble_prof_select(req->index) == 0;
  } else {
    LOG_WRN("Invalid profile index: %d", req->index);
  }
#endif
  return 0;
}

static int
handle_unpair_profile(const zmk_ble_management_UnpairProfileRequest *req,
                      zmk_ble_management_Response *resp) {
  LOG_DBG("UnpairProfileRequest: index=%d", req->index);
  resp->which_response_type = zmk_ble_management_Response_unpair_profile_tag;

#if IS_ENABLED(CONFIG_ZMK_BLE)
  if (req->index >= ZMK_BLE_PROFILE_COUNT) {
    LOG_WRN("Invalid profile index: %d", req->index);
    return 0;
  }
  bt_addr_le_t peer;
  if (!profile_peer_matches(req->index, req->has_expected_peer,
                            &req->expected_peer, &peer)) {
    set_error(resp, "Profile changed or identity missing; refresh profiles");
    return 0;
  }
  /* ZMK clears only the active profile's bond. */
  const int active = zmk_ble_active_profile_index();
  int rc = zmk_ble_prof_select(req->index);
  if (rc == 0) {
    /* Selection can wait for an HCI response. Recheck that boundary before
     * calling the core's active-profile-only clear API. The API itself has
     * no conditional index/peer transaction; concurrent changes inside that
     * call remain a core limitation. */
    if (zmk_ble_active_profile_index() != req->index ||
        !profile_peer_matches(req->index, req->has_expected_peer,
                              &req->expected_peer, NULL)) {
      set_error(resp, "Profile changed during selection; refresh profiles");
      return 0;
    }
    zmk_ble_clear_bonds();
    int label_rc = delete_profile_name(&peer);
    if (label_rc < 0) {
      LOG_WRN("Profile label not deleted: %d", label_rc);
    }
    if (active != req->index) {
      rc = zmk_ble_prof_select(active);
    }
  }
  resp->response_type.unpair_profile.success = (rc == 0);
#endif
  return 0;
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
/* A central is the Bluetooth central of its peripherals and a Bluetooth
 * peripheral to its hosts, so a connected central-role link is a half. */
static void find_peripheral_link(struct bt_conn *conn, void *data) {
  struct bt_conn_info info;
  if (bt_conn_get_info(conn, &info) == 0 && info.role == BT_CONN_ROLE_CENTRAL &&
      info.state == BT_CONN_STATE_CONNECTED) {
    *(bool *)data = true;
  }
}
#endif

static int handle_get_split_info(zmk_ble_management_Response *resp) {
  LOG_DBG("GetSplitInfoRequest");
  zmk_ble_management_GetSplitInfoResponse *result =
      &resp->response_type.get_split_info;
  resp->which_response_type = zmk_ble_management_Response_get_split_info_tag;
  /* Present even on a standalone keyboard, with every field false. */
  result->has_info = true;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
  /* Studio RPC runs only on the central (see Kconfig), so central_bonded,
   * a peripheral's view, stays false. */
  result->info.is_split = true;
  result->info.is_central = true;
  bt_conn_foreach(BT_CONN_TYPE_LE, find_peripheral_link,
                  &result->info.peripheral_connected);
#endif
  return 0;
}

static int handle_forget_split_bond(zmk_ble_management_Response *resp) {
  LOG_DBG("ForgetSplitBondRequest");
  resp->which_response_type = zmk_ble_management_Response_forget_split_bond_tag;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
  /* ZMK clears every host profile's bond and selects profile 0; the bond
   * between the halves is not touched. Labels follow the host bonds. */
  bt_addr_le_t peers[ZMK_BLE_PROFILE_COUNT];
  for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
    const bt_addr_le_t *addr = profile_peer(i);
    bt_addr_le_copy(&peers[i], addr ? addr : BT_ADDR_LE_NONE);
  }
  zmk_ble_clear_all_bonds();
  for (uint8_t i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
    if (!bt_addr_le_eq(&peers[i], BT_ADDR_LE_NONE)) {
      delete_profile_name(&peers[i]);
    }
  }
  resp->response_type.forget_split_bond.success = true;
#else
  LOG_WRN("Split BLE not enabled");
#endif
  return 0;
}

static int handle_set_output_priority(
    const zmk_ble_management_SetOutputPriorityRequest *req,
    zmk_ble_management_Response *resp) {
  LOG_DBG("SetOutputPriorityRequest: priority=%d", req->priority);
  resp->which_response_type =
      zmk_ble_management_Response_set_output_priority_tag;

  enum zmk_transport transport;
  switch (req->priority) {
  case zmk_ble_management_OutputPriority_OUTPUT_PRIORITY_USB:
    transport = ZMK_TRANSPORT_USB;
    break;
  case zmk_ble_management_OutputPriority_OUTPUT_PRIORITY_BLE:
    transport = ZMK_TRANSPORT_BLE;
    break;
  default:
    LOG_WRN("Invalid output priority: %d", req->priority);
    return 0;
  }
  resp->response_type.set_output_priority.success =
      zmk_endpoint_set_preferred_transport(transport) == 0;
  return 0;
}

static int handle_get_output_priority(zmk_ble_management_Response *resp) {
  LOG_DBG("GetOutputPriorityRequest");
  resp->which_response_type =
      zmk_ble_management_Response_get_output_priority_tag;

  enum zmk_transport current = zmk_endpoint_get_preferred_transport();
  if (current == ZMK_TRANSPORT_USB) {
    resp->response_type.get_output_priority.priority =
        zmk_ble_management_OutputPriority_OUTPUT_PRIORITY_USB;
  } else {
    if (current != ZMK_TRANSPORT_BLE) {
      LOG_WRN("Unknown transport type: %d", current);
    }
    resp->response_type.get_output_priority.priority =
        zmk_ble_management_OutputPriority_OUTPUT_PRIORITY_BLE;
  }
  return 0;
}

static bool
ble_management_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                  pb_callback_t *encode_response) {
  zmk_ble_management_Response *resp =
      ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(cormoran_ble,
                                                        encode_response);

  zmk_ble_management_Request req = zmk_ble_management_Request_init_zero;
  pb_istream_t req_stream = pb_istream_from_buffer(raw_request->payload.bytes,
                                                   raw_request->payload.size);
  if (!pb_decode(&req_stream, zmk_ble_management_Request_fields, &req)) {
    LOG_WRN("Failed to decode ble_management request: %s",
            PB_GET_ERROR(&req_stream));
    set_error(resp, "Failed to decode request");
    return true;
  }

  if (request_needs_unlock(req.which_request_type) && !studio_unlocked()) {
    LOG_WRN("BLE management change refused: Studio is locked");
    set_error(resp, "Unlock required");
    return true;
  }

  int rc;
  switch (req.which_request_type) {
  case zmk_ble_management_Request_get_profiles_tag:
    rc = handle_get_profiles(resp);
    break;
  case zmk_ble_management_Request_set_profile_name_tag:
    rc = handle_set_profile_name(&req.request_type.set_profile_name, resp);
    break;
  case zmk_ble_management_Request_switch_profile_tag:
    rc = handle_switch_profile(&req.request_type.switch_profile, resp);
    break;
  case zmk_ble_management_Request_unpair_profile_tag:
    rc = handle_unpair_profile(&req.request_type.unpair_profile, resp);
    break;
  case zmk_ble_management_Request_get_split_info_tag:
    rc = handle_get_split_info(resp);
    break;
  case zmk_ble_management_Request_forget_split_bond_tag:
    rc = handle_forget_split_bond(resp);
    break;
  case zmk_ble_management_Request_set_output_priority_tag:
    rc =
        handle_set_output_priority(&req.request_type.set_output_priority, resp);
    break;
  case zmk_ble_management_Request_get_output_priority_tag:
    rc = handle_get_output_priority(resp);
    break;
  default:
    LOG_WRN("Unsupported request type: %d", req.which_request_type);
    rc = -ENOTSUP;
  }

  if (rc != 0) {
    char message[sizeof(resp->response_type.error.message)];
    snprintf(message, sizeof(message), "Failed to process request: %d", rc);
    set_error(resp, message);
  }
  return true;
}
