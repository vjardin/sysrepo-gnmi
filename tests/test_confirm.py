"""
Confirmed-commit conformance tests.
Tests the Set + Confirm RPC interaction.
"""

import json
import sys
import os
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "proto"))

import grpc
import pytest
import gnmi_pb2
import gnmi_pb2_grpc

from helpers import xpath_to_path


def _set_with_confirm(gnmi_stub, timeout_secs=60):
    """Helper: do a Set with confirm_parms, return response."""
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='ConfirmTest']/description"
    )
    confirm = gnmi_pb2.ConfirmParmsRequest(timeout_secs=timeout_secs)
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"tentative"'),
        )],
        confirm=confirm,
    )
    return gnmi_stub.Set(req, timeout=5)


def _clear_confirm_state(gnmi_stub):
    """Helper: clear any pending confirm state."""
    try:
        gnmi_stub.Confirm(gnmi_pb2.ConfirmRequest(), timeout=5)
    except grpc.RpcError:
        pass  # FAILED_PRECONDITION or UNAVAILABLE - both fine


def _cleanup_confirm_data(gnmi_stub):
    """Helper: clear confirm state and delete test data."""
    _clear_confirm_state(gnmi_stub)
    path = xpath_to_path("/gnmi-server-test:test/things[name='ConfirmTest']")
    try:
        gnmi_stub.Set(gnmi_pb2.SetRequest(delete=[path]), timeout=5)
    except grpc.RpcError:
        pass


def test_confirm_before_timeout(gnmi_stub):
    """Set with confirm_parms, then Confirm before timeout.

    Flow: Set(confirm) -> data written -> Confirm -> data persisted to startup.
    """
    _cleanup_confirm_data(gnmi_stub)

    # Set with confirm_parms
    resp = _set_with_confirm(gnmi_stub, timeout_secs=60)
    assert resp.timestamp > 0
    assert resp.confirm is not None
    assert resp.confirm.timeout_secs > 0

    # Verify data is in running (via Get CONFIG)
    get_req = gnmi_pb2.GetRequest(
        path=[xpath_to_path(
            "/gnmi-server-test:test/things[name='ConfirmTest']/description"
        )],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    get_resp = gnmi_stub.Get(get_req, timeout=5)
    assert len(get_resp.notification[0].update) == 1
    val = get_resp.notification[0].update[0].val.json_ietf_val.decode()
    assert "tentative" in val

    # Confirm the Set (min_wait=0 in test mode, so immediate Confirm works)
    confirm_req = gnmi_pb2.ConfirmRequest()
    gnmi_stub.Confirm(confirm_req, timeout=5)

    _cleanup_confirm_data(gnmi_stub)


def test_confirm_without_set(gnmi_stub):
    """Confirm without prior Set - should fail with FAILED_PRECONDITION."""
    # Make sure no pending confirm
    _cleanup_confirm_data(gnmi_stub)

    confirm_req = gnmi_pb2.ConfirmRequest()
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Confirm(confirm_req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.FAILED_PRECONDITION
    assert "Not waiting" in exc_info.value.details()


def test_set_while_waiting_confirm(gnmi_stub):
    """Set while already waiting for Confirm - should fail.

    A second Set without confirm_parms while the first is pending
    should return FAILED_PRECONDITION.
    """
    _cleanup_confirm_data(gnmi_stub)

    # First Set with confirm
    resp = _set_with_confirm(gnmi_stub, timeout_secs=60)
    assert resp.confirm is not None

    # Second Set without confirm - should be rejected
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='Other']/description"
    )
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"blocked"'),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.FAILED_PRECONDITION

    _cleanup_confirm_data(gnmi_stub)


def test_confirm_timeout_rollback(gnmi_stub):
    """Set with short timeout, let it expire, verify rollback.

    Set with confirm(timeout=3s) -> write data -> wait 5s -> data rolled back.
    """
    _cleanup_confirm_data(gnmi_stub)

    # Ensure no pre-existing data
    get_req = gnmi_pb2.GetRequest(
        path=[xpath_to_path(
            "/gnmi-server-test:test/things[name='ConfirmTest']"
        )],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    get_resp = gnmi_stub.Get(get_req, timeout=5)
    assert len(get_resp.notification[0].update) == 0

    # Set with very short confirm timeout
    resp = _set_with_confirm(gnmi_stub, timeout_secs=3)
    assert resp.confirm is not None

    # Verify data exists now
    get_resp = gnmi_stub.Get(get_req, timeout=5)
    assert len(get_resp.notification[0].update) == 1

    # Wait for timeout to expire (3s + margin)
    time.sleep(5)

    # Verify data was rolled back
    get_resp = gnmi_stub.Get(get_req, timeout=5)
    assert len(get_resp.notification[0].update) == 0, \
        "Data should have been rolled back after confirm timeout"
