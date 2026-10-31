/*
 * Copyright (c) 2026 cormoran
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/bluetooth/addr.h>

/* Host-profile labels, kept per bonded address (src/profile_labels.c, built
 * with CONFIG_ZMK_BLE). Changes run on the Studio RPC thread; loading runs in
 * settings_load(), before any RPC. */

/* ProfileInfo.name / SetProfileNameRequest.name max_size. */
#define PROFILE_NAME_SIZE 32

/* The label of `addr`, or "" if it has none. */
const char *get_profile_name(const bt_addr_le_t *addr);

/* Stores `name` for `addr`; an empty name deletes the label. When every slot
 * is taken, the label of an address no profile is bonded to any more is
 * reclaimed. RAM is updated before storage, so a persistence error can leave
 * the new label in RAM until reboot. */
int save_profile_name(const bt_addr_le_t *addr, const char *name);

/* Removes the label of `addr` from RAM and storage, in both key formats. */
int delete_profile_name(const bt_addr_le_t *addr);
