"""
Helper functions for gNMI path conversion, mirroring
path conversion utilities.
"""

import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "proto"))
import gnmi_pb2


def xpath_to_path(xpath: str) -> gnmi_pb2.Path:
    """Convert an XPath-like string to a gNMI Path with origin rfc7951.

    Example: "/gnmi-server-test:test/things[name='A']"
    """
    path = gnmi_pb2.Path(origin="rfc7951")
    if not xpath or xpath == "/":
        return path

    # Strip leading /
    if xpath.startswith("/"):
        xpath = xpath[1:]

    for segment in _split_path(xpath):
        name, keys = _parse_segment(segment)
        elem = gnmi_pb2.PathElem(name=name, key=keys)
        path.elem.append(elem)

    return path


def path_to_xpath(path: gnmi_pb2.Path) -> str:
    """Convert a gNMI Path to an XPath-like string."""
    if not path.elem:
        return "/"
    parts = []
    for elem in path.elem:
        s = "/" + elem.name
        for k, v in sorted(elem.key.items()):
            s += f"[{k}='{v}']"
        parts.append(s)
    return "".join(parts)


def _split_path(xpath: str) -> list:
    """Split xpath by / but respect brackets."""
    segments = []
    current = ""
    depth = 0
    for ch in xpath:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        elif ch == "/" and depth == 0:
            if current:
                segments.append(current)
            current = ""
            continue
        current += ch
    if current:
        segments.append(current)
    return segments


def _parse_segment(segment: str) -> tuple:
    """Parse 'name[key1=val1][key2=val2]' into (name, {key: val})."""
    keys = {}
    bracket = segment.find("[")
    if bracket == -1:
        return segment, keys

    name = segment[:bracket]
    rest = segment[bracket:]

    while rest:
        if not rest.startswith("["):
            break
        eq = rest.index("=")
        key_name = rest[1:eq]
        # Find value between quotes
        if rest[eq + 1] == "'":
            end = rest.index("'", eq + 2)
            val = rest[eq + 2 : end]
            rest = rest[end + 2 :]  # skip ']
        elif rest[eq + 1] == '"':
            end = rest.index('"', eq + 2)
            val = rest[eq + 2 : end]
            rest = rest[end + 2 :]
        else:
            close = rest.index("]")
            val = rest[eq + 1 : close]
            rest = rest[close + 1 :]
        keys[key_name] = val

    return name, keys
