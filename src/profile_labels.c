/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * A label is stored as "ble_mgmt/name/<type>/<address>", the address type as
 * two hex digits. Older firmware stored "ble_mgmt/name/<address>" and those
 * keys are read as public addresses; a typed key wins over a legacy one in
 * either load order. A label follows its host's bond: it is deleted when the
 * profile is unpaired, and reclaimed once no profile is bonded to it.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/settings/settings.h>
#include <zmk/ble.h>

#include "profile_labels.h"

#include <zephyr/logging/log.h>
#ifndef CONFIG_ZMK_MODULE_BLE_MANAGEMENT_LOG_LEVEL
#define CONFIG_ZMK_MODULE_BLE_MANAGEMENT_LOG_LEVEL 3
#endif
LOG_MODULE_REGISTER(zmk_module_ble_management_profile_labels,
                    CONFIG_ZMK_MODULE_BLE_MANAGEMENT_LOG_LEVEL);

#define SETTINGS_SUBTREE "ble_mgmt"
#define SETTINGS_KEY_SIZE 64

struct profile_name_entry {
  bool used;
  /* The name came from a typed key, which a legacy key must not replace. */
  bool typed_key;
  /* A legacy key for this address exists in storage. */
  bool legacy_record;
  bt_addr_le_t addr;
  char name[PROFILE_NAME_SIZE];
};

static struct profile_name_entry profile_names[ZMK_BLE_PROFILE_COUNT];

static int find_entry(const bt_addr_le_t *addr) {
  for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
    if (profile_names[i].used && bt_addr_le_eq(&profile_names[i].addr, addr)) {
      return i;
    }
  }
  return -ENOENT;
}

static int find_free_entry(void) {
  for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
    if (!profile_names[i].used) {
      return i;
    }
  }
  return -ENOMEM;
}

static void format_key(char *key, const bt_addr_le_t *addr, bool typed) {
  char addr_str[BT_ADDR_STR_LEN];
  bt_addr_to_str(&addr->a, addr_str, sizeof(addr_str));
  if (typed) {
    snprintf(key, SETTINGS_KEY_SIZE, SETTINGS_SUBTREE "/name/%02x/%s",
             addr->type, addr_str);
  } else {
    snprintf(key, SETTINGS_KEY_SIZE, SETTINGS_SUBTREE "/name/%s", addr_str);
  }
}

/* Deletes one stored key; a key that does not exist is not an error. */
static int delete_key(const bt_addr_le_t *addr, bool typed) {
  char key[SETTINGS_KEY_SIZE];
  format_key(key, addr, typed);
  int rc = settings_delete(key);
  return rc == -ENOENT ? 0 : rc;
}

/* Deletes every stored key of `addr`; only a public address can have a
 * legacy key. */
static int delete_records(const bt_addr_le_t *addr) {
  bt_addr_le_t peer;
  bt_addr_le_copy(&peer, addr);
  addr = &peer;
  int rc = delete_key(addr, true);
  if (addr->type == BT_ADDR_LE_PUBLIC) {
    int legacy_rc = delete_key(addr, false);
    if (rc == 0) {
      rc = legacy_rc;
    }
  }
  return rc;
}

/* Every slot is taken: reuse the one whose address no profile is bonded to
 * any more, e.g. after its host was unpaired by a &bt key binding. */
static int reclaim_stale_entry(void) {
  for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
    if (zmk_ble_profile_index(&profile_names[i].addr) < 0) {
      int rc = delete_records(&profile_names[i].addr);
      if (rc < 0) {
        LOG_WRN("Stale profile label not deleted: %d", rc);
        return rc;
      }
      profile_names[i] = (struct profile_name_entry){0};
      return i;
    }
  }
  return -ENOMEM;
}

const char *get_profile_name(const bt_addr_le_t *addr) {
  int i = addr ? find_entry(addr) : -ENOENT;
  return i >= 0 ? profile_names[i].name : "";
}

int delete_profile_name(const bt_addr_le_t *addr) {
  if (!addr) {
    return -EINVAL;
  }
  bt_addr_le_t peer;
  bt_addr_le_copy(&peer, addr);
  addr = &peer;
  int i = find_entry(addr);
  if (i >= 0) {
    profile_names[i] = (struct profile_name_entry){0};
  }
  return delete_records(addr);
}

int save_profile_name(const bt_addr_le_t *addr, const char *name) {
  if (!addr || !name) {
    return -EINVAL;
  }
  bt_addr_le_t peer;
  bt_addr_le_copy(&peer, addr);
  addr = &peer;
  if (addr->type > BT_ADDR_LE_RANDOM_ID) {
    return -EINVAL;
  }
  if (name[0] == '\0') {
    return delete_profile_name(addr);
  }

  int i = find_entry(addr);
  if (i < 0) {
    i = find_free_entry();
  }
  if (i < 0) {
    i = reclaim_stale_entry();
  }
  if (i < 0) {
    LOG_WRN("No slot available for profile name");
    return i;
  }

  struct profile_name_entry *entry = &profile_names[i];
  const bool legacy_record = entry->used && entry->legacy_record;
  entry->used = true;
  entry->typed_key = true;
  entry->legacy_record = false;
  bt_addr_le_copy(&entry->addr, addr);
  strncpy(entry->name, name, sizeof(entry->name) - 1);
  entry->name[sizeof(entry->name) - 1] = '\0';

  char key[SETTINGS_KEY_SIZE];
  format_key(key, addr, true);
  int rc = settings_save_one(key, entry->name, strlen(entry->name) + 1);
  if (rc == 0 && legacy_record) {
    /* The typed key now wins, so the legacy one is dead weight. */
    rc = delete_key(addr, false);
  }
  return rc;
}

static int profile_names_settings_set(const char *name, size_t len,
                                      settings_read_cb read_cb, void *cb_arg) {
  const char *next;
  if (!settings_name_steq(name, "name", &next) || !next) {
    return 0;
  }

  const char *addr_str = next;
  bool typed_key = false;
  bt_addr_le_t addr = {.type = BT_ADDR_LE_PUBLIC};
  size_t key_len = strlen(next);
  if (key_len == BT_ADDR_STR_LEN + 2 && next[0] == '0' && next[1] >= '0' &&
      next[1] <= '3' && next[2] == '/') {
    addr.type = (uint8_t)(next[1] - '0');
    addr_str = next + 3;
    typed_key = true;
  } else if (key_len != BT_ADDR_STR_LEN - 1) {
    return 0;
  }
  /* Legacy keys omitted the type and were always restored as public. */
  if (bt_addr_from_str(addr_str, &addr.a) != 0) {
    LOG_WRN("Failed to parse profile name address");
    return 0;
  }

  /* Read privately: a failed or partial read must not alter the cache. */
  char loaded_name[PROFILE_NAME_SIZE];
  if (len == 0 || len > sizeof(loaded_name)) {
    return -EINVAL;
  }
  int rc = read_cb(cb_arg, loaded_name, len);
  if (rc < 0) {
    return rc;
  }
  if ((size_t)rc != len) {
    return -EIO;
  }
  if (memchr(loaded_name, '\0', len) != &loaded_name[len - 1]) {
    return -EINVAL;
  }

  int i = find_entry(&addr);
  if (i < 0) {
    i = find_free_entry();
  }
  if (i < 0) {
    LOG_WRN("No slot for loading profile name");
    return -ENOMEM;
  }

  struct profile_name_entry *entry = &profile_names[i];
  if (!typed_key) {
    entry->legacy_record = true;
    if (entry->used && entry->typed_key) {
      return 0;
    }
  }
  entry->used = true;
  entry->typed_key = typed_key;
  bt_addr_le_copy(&entry->addr, &addr);
  memcpy(entry->name, loaded_name, len);
  LOG_DBG("profile label loaded");
  return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(ble_mgmt, SETTINGS_SUBTREE, NULL,
                               profile_names_settings_set, NULL, NULL);
