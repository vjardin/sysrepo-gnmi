"""
NACM (RFC 8341) enforcement tests.

Uses a separate gnmi_server instance started with -u nacmuser on port
40052, to verify that sysrepo NACM rules are enforced through
sr_session_set_user().
"""

import os
import sys
import time
import signal
import socket
import subprocess

import grpc
import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "proto"))
import gnmi_pb2
import gnmi_pb2_grpc
from helpers import xpath_to_path

NACM_BIND = "localhost:40052"
NACM_USER = "nacmtestuser"


def _wait_for_port(host, port, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except (ConnectionRefusedError, OSError):
            time.sleep(0.1)
    return False


@pytest.fixture(scope="module")
def nacm_env(gnmi_env):
    """Reuse the session-wide env but return it for NACM server."""
    return gnmi_env


@pytest.fixture(scope="module")
def nacm_setup(nacm_env, gnmi_stub):
    """Configure NACM via the admin (no-user) server.

    Sets global defaults: read=permit, write=deny, exec=deny.
    The nacmtestuser is a non-recovery user so these defaults apply.
    """
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[
            gnmi_pb2.Update(
                path=xpath_to_path("/ietf-netconf-acm:nacm/enable-nacm"),
                val=gnmi_pb2.TypedValue(json_ietf_val=b'true'),
            ),
            gnmi_pb2.Update(
                path=xpath_to_path("/ietf-netconf-acm:nacm/read-default"),
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"permit"'),
            ),
            gnmi_pb2.Update(
                path=xpath_to_path("/ietf-netconf-acm:nacm/write-default"),
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"deny"'),
            ),
            gnmi_pb2.Update(
                path=xpath_to_path("/ietf-netconf-acm:nacm/exec-default"),
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"deny"'),
            ),
        ],
    ), timeout=5)

    yield

    # Cleanup: restore permissive defaults
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[
            gnmi_pb2.Update(
                path=xpath_to_path("/ietf-netconf-acm:nacm/enable-nacm"),
                val=gnmi_pb2.TypedValue(json_ietf_val=b'false'),
            ),
            gnmi_pb2.Update(
                path=xpath_to_path("/ietf-netconf-acm:nacm/write-default"),
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"permit"'),
            ),
            gnmi_pb2.Update(
                path=xpath_to_path("/ietf-netconf-acm:nacm/exec-default"),
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"permit"'),
            ),
        ],
    ), timeout=5)


@pytest.fixture(scope="module")
def nacm_server(nacm_env, nacm_setup, seed_oper):
    """Start a gnmi_server with -u nacmtestuser on port 40052."""
    env, build_dir = nacm_env
    binary = os.environ.get(
        "GNMI_SERVER_BIN",
        os.path.join(build_dir, "gnmi_server"),
    )
    if not os.path.isfile(binary):
        pytest.skip(f"gnmi_server binary not found: {binary}")

    log_path = os.path.join(build_dir, "gnmi_server_nacm.log")
    log_file = open(log_path, "w")

    cmd = [binary, "-f", "-b", NACM_BIND, "-l", "4", "-u", NACM_USER]
    proc = subprocess.Popen(cmd, env=env, stdout=log_file, stderr=log_file)

    host, port = NACM_BIND.split(":")
    if not _wait_for_port(host, int(port), timeout=10):
        proc.kill()
        log_file.close()
        with open(log_path) as f:
            pytest.fail(f"nacm gnmi_server failed to start:\n{f.read()}")

    yield proc

    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    log_file.close()


@pytest.fixture(scope="module")
def nacm_stub(nacm_server):
    """Create gNMI stub connected to the NACM-restricted server."""
    os.environ["no_proxy"] = "localhost,127.0.0.1"
    channel = grpc.insecure_channel(NACM_BIND)
    stub = gnmi_pb2_grpc.gNMIStub(channel)
    yield stub
    channel.close()


# -- Tests --


def test_nacm_read_permitted(nacm_stub):
    """NACM: read access is permitted (default read-default=permit).

    Equivalent to: "NACM read permitted" [nacm]
    """
    path = xpath_to_path("/gnmi-server-test:test-state")
    resp = nacm_stub.Get(gnmi_pb2.GetRequest(
        path=[path], encoding=gnmi_pb2.JSON_IETF,
    ), timeout=5)
    assert len(resp.notification) == 1


@pytest.mark.skipif(os.getuid() != 0, reason="NACM enforcement requires root")
def test_nacm_write_denied(nacm_stub):
    """NACM: write access is denied by rule.

    Equivalent to: "NACM write denied" [nacm]
    Requires root: sysrepo skips NACM enforcement for non-root processes.
    """
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='NACMWrite']/description"
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        nacm_stub.Set(gnmi_pb2.SetRequest(
            update=[gnmi_pb2.Update(
                path=path,
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"blocked"'),
            )],
        ), timeout=5)
    assert exc_info.value.code() in (
        grpc.StatusCode.PERMISSION_DENIED,
        grpc.StatusCode.ABORTED,
    )


@pytest.mark.skipif(os.getuid() != 0, reason="NACM enforcement requires root")
def test_nacm_exec_denied(nacm_stub):
    """NACM: exec (RPC) access is denied by rule.

    Equivalent to: "NACM exec denied" [nacm]
    Requires root: sysrepo skips NACM enforcement for non-root processes.
    """
    path = xpath_to_path("/gnmi-server-test:clear-stats")
    with pytest.raises(grpc.RpcError) as exc_info:
        nacm_stub.Rpc(gnmi_pb2.RpcRequest(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'{}'),
        ), timeout=5)
    assert exc_info.value.code() in (
        grpc.StatusCode.PERMISSION_DENIED,
        grpc.StatusCode.ABORTED,
    )


def test_nacm_capabilities_permitted(nacm_stub):
    """NACM: Capabilities is always permitted regardless of rules.

    Equivalent to: "NACM capabilities permitted" [nacm]
    """
    resp = nacm_stub.Capabilities(gnmi_pb2.CapabilityRequest(), timeout=5)
    assert resp.gNMI_version == "0.7.0"
