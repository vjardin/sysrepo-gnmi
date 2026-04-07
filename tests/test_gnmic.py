"""
gnmic CLI interoperability tests.

Validates that gnmic (OpenConfig gNMI CLI client) works correctly
against sysrepo-gnmi for all implemented features.
"""

import json
import os
import subprocess
import sys
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "proto"))
import gnmi_pb2
from helpers import xpath_to_path

GNMIC = "gnmic"
ADDR = "localhost:40051"
GNMIC_BASE = [GNMIC, "-a", ADDR, "--insecure"]


@pytest.fixture(scope="module", autouse=True)
def check_gnmic():
    """Skip all tests if gnmic is not installed."""
    try:
        subprocess.run([GNMIC, "version"], capture_output=True, check=True)
    except (FileNotFoundError, subprocess.CalledProcessError):
        pytest.skip("gnmic not installed")


def _gnmic(*args, timeout=10):
    """Run a gnmic command, return parsed JSON output."""
    cmd = GNMIC_BASE + list(args)
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(f"gnmic failed: {result.stderr}")
    return json.loads(result.stdout) if result.stdout.strip() else None


def _gnmic_raw(*args, timeout=10):
    """Run a gnmic command, return raw stdout."""
    cmd = GNMIC_BASE + list(args)
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return result


def test_gnmic_capabilities(gnmi_server):
    """gnmic capabilities returns gNMI version and models."""
    result = _gnmic_raw("capabilities")
    assert result.returncode == 0
    assert "0.7.0" in result.stdout
    assert "gnmi-server-test" in result.stdout


def test_gnmic_get_json_ietf(gnmi_server):
    """gnmic get with JSON_IETF encoding returns operational data."""
    data = _gnmic("get", "--path", "/gnmi-server-test:test-state",
                  "-e", "json_ietf")
    assert len(data) >= 1
    updates = data[0].get("updates", [])
    assert len(updates) >= 1
    vals = updates[0].get("values", {})
    # Should contain test-state with things
    assert any("test-state" in k for k in vals)


def test_gnmic_get_config(gnmi_server, gnmi_stub):
    """gnmic get with CONFIG type returns config data."""
    # Create test data
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=xpath_to_path(
                "/gnmi-server-test:test/things[name='gnmicGet']/description"
            ),
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"gnmic-config"'),
        )],
    ), timeout=5)

    data = _gnmic("get",
                  "--path", "/gnmi-server-test:test/things[name=gnmicGet]/description",
                  "-e", "json_ietf", "--type", "CONFIG")
    assert len(data) >= 1
    updates = data[0].get("updates", [])
    assert len(updates) >= 1
    assert "gnmic-config" in json.dumps(updates)

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test/things[name='gnmicGet']")],
    ), timeout=5)


def test_gnmic_set_update_delete(gnmi_server):
    """gnmic set update then delete."""
    # Update
    result = _gnmic_raw("set", "-e", "json_ietf",
                        "--update-path", "/gnmi-server-test:test/things[name=gnmicSet]/description",
                        "--update-value", '"gnmic-val"')
    assert result.returncode == 0
    assert "UPDATE" in result.stdout

    # Verify
    data = _gnmic("get",
                  "--path", "/gnmi-server-test:test/things[name=gnmicSet]/description",
                  "-e", "json_ietf", "--type", "CONFIG")
    assert "gnmic-val" in json.dumps(data)

    # Delete
    result = _gnmic_raw("set",
                        "--delete", "/gnmi-server-test:test/things[name=gnmicSet]")
    assert result.returncode == 0
    assert "DELETE" in result.stdout


def test_gnmic_set_replace(gnmi_server, gnmi_stub):
    """gnmic set replace operation."""
    # Create data first
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=xpath_to_path(
                "/gnmi-server-test:test/things[name='gnmicRepl']/description"
            ),
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"original"'),
        )],
    ), timeout=5)

    # Replace via gnmic
    result = _gnmic_raw("set", "-e", "json_ietf",
                        "--replace-path", "/gnmi-server-test:test/things[name=gnmicRepl]/description",
                        "--replace-value", '"replaced"')
    assert result.returncode == 0
    assert "REPLACE" in result.stdout

    # Verify
    data = _gnmic("get",
                  "--path", "/gnmi-server-test:test/things[name=gnmicRepl]/description",
                  "-e", "json_ietf", "--type", "CONFIG")
    assert "replaced" in json.dumps(data)

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test/things[name='gnmicRepl']")],
    ), timeout=5)


def test_gnmic_subscribe_once(gnmi_server):
    """gnmic subscribe once returns data + sync."""
    result = _gnmic_raw("subscribe",
                        "--path", "/gnmi-server-test:test-state",
                        "--mode", "once", "-e", "json_ietf",
                        timeout=10)
    assert result.returncode == 0
    assert "sync-response" in result.stdout
    assert "test-state" in result.stdout


def test_gnmic_subscribe_stream_sample(gnmi_server):
    """gnmic subscribe stream sample receives multiple updates."""
    try:
        result = subprocess.run(
            GNMIC_BASE + [
                "subscribe",
                "--path", "/gnmi-server-test:test-state",
                "--mode", "stream",
                "--stream-mode", "sample",
                "--sample-interval", "1s",
                "-e", "json_ietf",
            ],
            capture_output=True, text=True, timeout=4,
        )
    except subprocess.TimeoutExpired as e:
        # Expected: gnmic keeps streaming until timeout
        stdout = e.stdout.decode() if e.stdout else ""
        assert "test-state" in stdout
        assert stdout.count('"updates"') >= 2, \
            f"Expected >=2 sample updates, got {stdout.count('updates')}"
        return

    # If it returned before timeout, still check output
    assert "test-state" in result.stdout


# -- Depth extension --

def test_gnmic_get_depth(gnmi_server):
    """gnmic get with --depth 1 returns only direct children."""
    data = _gnmic("get", "--path", "/gnmi-server-test:test-state",
                  "-e", "json_ietf", "--depth", "1")
    assert len(data) >= 1
    updates = data[0].get("updates", [])
    assert len(updates) >= 1
    vals_json = json.dumps(updates[0].get("values", {}))
    # depth 1: should have "things" key but NOT nested "counter" values
    assert "things" in vals_json
    assert "counter" not in vals_json, \
        f"depth=1 should not include nested counter, got: {vals_json}"


def test_gnmic_get_no_depth(gnmi_server):
    """gnmic get without --depth returns full tree including nested leaves."""
    data = _gnmic("get", "--path", "/gnmi-server-test:test-state",
                  "-e", "json_ietf")
    assert len(data) >= 1
    updates = data[0].get("updates", [])
    vals_json = json.dumps(updates[0].get("values", {}))
    # No depth limit: should include nested "counter" values
    assert "counter" in vals_json
