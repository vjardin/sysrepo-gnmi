"""
Rpc RPC conformance tests.
"""

import json
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "proto"))

import grpc
import pytest
import gnmi_pb2
import gnmi_pb2_grpc

from helpers import xpath_to_path


def test_rpc(gnmi_stub):
    """Rpc - normal invocation with input/output.

    Equivalent to: "Rpc" [rpc]
    Calls /gnmi-server-test:clear-stats with input {"interface": "eth45"}.
    Expects output {"old-stats": "613"}.
    """
    req = gnmi_pb2.RpcRequest()
    req.path.CopyFrom(xpath_to_path("/gnmi-server-test:clear-stats"))
    req.val.json_ietf_val = json.dumps({"interface": "eth45"}).encode()
    req.encoding = gnmi_pb2.JSON_IETF

    resp = gnmi_stub.Rpc(req, timeout=5)
    assert resp.timestamp > 0
    assert resp.val.json_ietf_val
    out = resp.val.json_ietf_val.decode()
    assert "613" in out


def test_rpc_empty_error(gnmi_stub):
    """Rpc request (empty error) - input with empty leaf still succeeds.

    Equivalent to: "Rpc request (empty error)" [rpc-neg]
    Input with {"interface": "none", "errors": [null]}.
    """
    req = gnmi_pb2.RpcRequest()
    req.path.CopyFrom(xpath_to_path("/gnmi-server-test:clear-stats"))
    req.val.json_ietf_val = json.dumps(
        {"interface": "none", "errors": [None]}
    ).encode()
    req.encoding = gnmi_pb2.JSON_IETF

    resp = gnmi_stub.Rpc(req, timeout=5)
    assert resp.timestamp > 0
    out = resp.val.json_ietf_val.decode()
    assert "613" in out


def test_rpc_no_val_type(gnmi_stub):
    """Rpc request (no val type) - missing value.

    Equivalent to: "Rpc request (no val type)" [rpc-neg]
    """
    req = gnmi_pb2.RpcRequest()
    req.path.CopyFrom(xpath_to_path("/gnmi-server-test:clear-stats"))
    # val not set - VALUE_NOT_SET

    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Rpc(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT
    assert "Value not set" in exc_info.value.details()


def test_rpc_error(gnmi_stub):
    """Rpc request (error) - callback returns error.

    Equivalent to: "Rpc request (error)" [rpc-neg]
    Input {"interface": "error"} triggers callback to return error.
    """
    req = gnmi_pb2.RpcRequest()
    req.path.CopyFrom(xpath_to_path("/gnmi-server-test:clear-stats"))
    req.val.json_ietf_val = json.dumps({"interface": "error"}).encode()
    req.encoding = gnmi_pb2.JSON_IETF

    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Rpc(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.ABORTED
    assert "Fiddlesticks" in exc_info.value.details()


def test_rpc_timeout(gnmi_stub):
    """Rpc request (timeout) - callback sleeps past timeout.

    Equivalent to: "Rpc request (timeout)" [rpc-neg]
    Input {"interface": "timeout"} triggers callback to sleep 3s.
    Default timeout is 2s -> should fail.
    """
    req = gnmi_pb2.RpcRequest()
    req.path.CopyFrom(xpath_to_path("/gnmi-server-test:clear-stats"))
    req.val.json_ietf_val = json.dumps({"interface": "timeout"}).encode()
    req.encoding = gnmi_pb2.JSON_IETF
    # Don't set req.timeout - default 2s, callback sleeps 3s

    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Rpc(req, timeout=10)  # gRPC timeout longer than SR timeout
    assert exc_info.value.code() == grpc.StatusCode.ABORTED
    assert "timed out" in exc_info.value.details().lower()


def test_get_schema_known_module(gnmi_stub):
    """get-schema: fetch a known YANG module by name.

    Retrieves the gnmi-server-test module and verifies it contains
    expected YANG keywords.
    """
    req = gnmi_pb2.RpcRequest()
    req.path.CopyFrom(
        xpath_to_path("/sysrepo-gnmi-monitoring:get-schema")
    )
    req.val.json_ietf_val = json.dumps(
        {"identifier": "gnmi-server-test"}
    ).encode()
    req.encoding = gnmi_pb2.JSON_IETF

    resp = gnmi_stub.Rpc(req, timeout=5)
    assert resp.timestamp > 0
    # The response contains JSON-wrapped YANG source; parse to extract
    raw = resp.val.json_ietf_val.decode() if resp.val.json_ietf_val else ""
    # Response is JSON: {"sysrepo-gnmi-monitoring:schema": "module ..."}
    parsed = json.loads(raw) if raw else {}
    schema = parsed.get("sysrepo-gnmi-monitoring:schema",
                        parsed.get("schema", ""))
    assert "module" in schema, "Expected YANG 'module' keyword in schema"
    assert "gnmi-server-test" in schema, "Expected module name in schema"


def test_get_schema_monitoring_module(gnmi_stub):
    """get-schema: fetch the monitoring module itself."""
    req = gnmi_pb2.RpcRequest()
    req.path.CopyFrom(
        xpath_to_path("/sysrepo-gnmi-monitoring:get-schema")
    )
    req.val.json_ietf_val = json.dumps(
        {"identifier": "sysrepo-gnmi-monitoring"}
    ).encode()
    req.encoding = gnmi_pb2.JSON_IETF

    resp = gnmi_stub.Rpc(req, timeout=5)
    raw = resp.val.json_ietf_val.decode() if resp.val.json_ietf_val else ""
    parsed = json.loads(raw) if raw else {}
    schema = parsed.get("sysrepo-gnmi-monitoring:schema",
                        parsed.get("schema", ""))
    assert "kill-session" in schema, "Expected kill-session RPC in monitoring schema"
    assert "get-schema" in schema, "Expected get-schema RPC in monitoring schema"


def test_get_schema_not_found(gnmi_stub):
    """get-schema: request a non-existent module returns error."""
    req = gnmi_pb2.RpcRequest()
    req.path.CopyFrom(
        xpath_to_path("/sysrepo-gnmi-monitoring:get-schema")
    )
    req.val.json_ietf_val = json.dumps(
        {"identifier": "nonexistent-module-xyz"}
    ).encode()
    req.encoding = gnmi_pb2.JSON_IETF

    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Rpc(req, timeout=5)
    assert exc_info.value.code() != grpc.StatusCode.OK
