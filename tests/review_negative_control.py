#!/usr/bin/env python3
"""Restore one peer-lifetime defect in a disposable owner source copy."""

import argparse
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
SNAPSHOT = """bt_addr_le_t peer;
  bt_addr_le_copy(&peer, addr);
  addr = &peer;"""
UNPAIR_RECHECK = """if (zmk_ble_active_profile_index() != req->index ||
        !profile_peer_matches(req->index, req->has_expected_peer,
                              &req->expected_peer, NULL)) {
      set_error(resp, "Profile changed during selection; refresh profiles");
      return 0;
    }"""


def function_span(source, name):
    pattern = r"^(?:static\s+)?(?:int|bool)\s+" + re.escape(name) + r"\("
    matches = list(re.finditer(pattern, source, re.M))
    if len(matches) != 1:
        raise ValueError("Expected one production function: " + name)
    found = matches[0]
    opening = source.index("{", found.end())
    depth = 0
    for token in re.finditer(
        r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]',
        source[opening:],
        re.S,
    ):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return found.start(), opening + token.end()
    raise ValueError("Unterminated production function: " + name)


def replace_in_function(target, relative, name, before, after):
    path = target / relative
    if not path.resolve().is_relative_to(target):
        raise ValueError("Refusing source links outside the disposable owner")
    source = path.read_text()
    first, last = function_span(source, name)
    body = source[first:last]
    tokens = re.findall(
        r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|'
        r"\d+|->|\+\+|--|==|!=|<=|>=|&&|\|\||<<|>>|\S",
        before,
    )
    pattern = r"\s*".join(re.escape(token) for token in tokens)
    if len(list(re.finditer(pattern, body))) != 1:
        raise ValueError("Expected exactly one selected peer-lifetime boundary")
    body = re.sub(pattern, lambda _: after, body, count=1)
    path.write_text(source[:first] + body + source[last:])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "fault",
        choices=(
            "rename-reclaim",
            "label-save-peer",
            "label-delete-peer",
            "unpair-selection",
        ),
    )
    parser.add_argument("isolated_owner", type=Path)
    args = parser.parse_args()
    target = args.isolated_owner.resolve()
    owner = ROOT.resolve()
    if target == owner or target.is_relative_to(owner) or owner.is_relative_to(target):
        parser.error("Use a distinct disposable container owner source copy")
    if args.fault == "rename-reclaim":
        # Restore the old end-to-end borrowed path. The helper snapshot is a
        # second defense, so restoring only the call would not reproduce the
        # settings-reclaim lifetime defect.
        replace_in_function(
            target,
            "src/studio/ble_management_handler.c",
            "handle_set_profile_name",
            "save_profile_name(&peer, req->name)",
            "save_profile_name(profile_peer(req->index), req->name)",
        )
    if args.fault in ("rename-reclaim", "label-save-peer", "label-delete-peer"):
        replace_in_function(
            target,
            "src/profile_labels.c",
            "delete_profile_name"
            if args.fault == "label-delete-peer"
            else "save_profile_name",
            SNAPSHOT,
            "",
        )
        if args.fault == "label-delete-peer":
            replace_in_function(
                target,
                "src/profile_labels.c",
                "delete_records",
                SNAPSHOT,
                "",
            )
    else:
        replace_in_function(
            target,
            "src/studio/ble_management_handler.c",
            "handle_unpair_profile",
            UNPAIR_RECHECK,
            "/* Negative control omits the post-selection check. */",
        )
    print("Applied isolated peer-lifetime fault: " + args.fault)


if __name__ == "__main__":
    main()
