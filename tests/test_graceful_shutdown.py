"""
Graceful shutdown tests.

Spawns a dedicated gnmi_server, opens a subscribe stream, sends SIGTERM,
and verifies the stream gets a clean UNAVAILABLE status (not an abrupt
connection reset).
"""

import os
import signal
import socket
import subprocess
import sys
import time
import threading

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "proto"))

import grpc
import pytest
import gnmi_pb2
import gnmi_pb2_grpc

from helpers import xpath_to_path

# Use a different port to avoid conflict with the session-scoped server
SHUTDOWN_BIND = "localhost:40061"


def _wait_for_port(host, port, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except (ConnectionRefusedError, OSError):
            time.sleep(0.1)
    return False


@pytest.fixture
def shutdown_server(gnmi_env):
    """Start a dedicated gnmi_server for shutdown testing."""
    env, build_dir = gnmi_env
    binary = os.environ.get(
        "GNMI_SERVER_BIN",
        os.path.join(build_dir, "gnmi_server"),
    )
    if not os.path.isfile(binary):
        pytest.skip(f"gnmi_server binary not found: {binary}")

    log_path = os.path.join(build_dir, "gnmi_server_shutdown_test.log")
    log_file = open(log_path, "w")

    proc = subprocess.Popen(
        [binary, "-f", "-b", SHUTDOWN_BIND, "-l", "4"],
        env=env,
        stdout=log_file,
        stderr=log_file,
    )

    host, port = SHUTDOWN_BIND.split(":")
    if not _wait_for_port(host, int(port)):
        proc.kill()
        proc.wait()
        log_file.close()
        with open(log_path) as f:
            pytest.fail(f"shutdown_server failed to start:\n{f.read()}")

    yield proc

    # Ensure cleanup even if test fails to send SIGTERM
    if proc.poll() is None:
        proc.kill()
        proc.wait()
    log_file.close()


def test_graceful_shutdown_closes_stream(shutdown_server):
    """SIGTERM closes active subscribe streams with UNAVAILABLE, not a crash."""
    channel = grpc.insecure_channel(SHUTDOWN_BIND)
    stub = gnmi_pb2_grpc.gNMIStub(channel)

    # Open a STREAM/SAMPLE subscribe stream
    path = xpath_to_path("/gnmi-server-test:test-state")
    sub = gnmi_pb2.Subscription(
        path=path,
        mode=gnmi_pb2.SubscriptionMode.SAMPLE,
        sample_interval=1_000_000_000,  # 1s
    )
    sl = gnmi_pb2.SubscriptionList(
        subscription=[sub],
        mode=gnmi_pb2.SubscriptionList.STREAM,
        encoding=gnmi_pb2.JSON_IETF,
    )
    req = gnmi_pb2.SubscribeRequest(subscribe=sl)

    responses = stub.Subscribe(iter([req]))

    # Wait for initial data + sync_response
    got_sync = False
    for resp in responses:
        if resp.sync_response:
            got_sync = True
            break
    assert got_sync, "Expected sync_response from subscribe stream"

    # Send SIGTERM to the server
    shutdown_server.send_signal(signal.SIGTERM)

    # Read remaining responses -- we should get a clean gRPC error,
    # not a connection reset or hanging read.
    got_error = False
    error_code = None
    try:
        for resp in responses:
            pass  # drain any remaining responses
    except grpc.RpcError as e:
        got_error = True
        error_code = e.code()

    assert got_error, "Expected gRPC error after SIGTERM, but stream ended cleanly"
    assert error_code == grpc.StatusCode.UNAVAILABLE, (
        f"Expected UNAVAILABLE, got {error_code}"
    )

    # Server should exit cleanly
    rc = shutdown_server.wait(timeout=10)
    assert rc == 0, f"Server exited with code {rc}"

    channel.close()


def test_graceful_shutdown_unary_after_sigterm(shutdown_server):
    """After SIGTERM, the server exits cleanly even without active streams."""
    channel = grpc.insecure_channel(SHUTDOWN_BIND)
    stub = gnmi_pb2_grpc.gNMIStub(channel)

    # Verify server is alive with a Capabilities RPC
    resp = stub.Capabilities(gnmi_pb2.CapabilityRequest())
    assert resp.gNMI_version

    # Send SIGTERM
    shutdown_server.send_signal(signal.SIGTERM)

    # Server should exit cleanly within the drain timeout
    rc = shutdown_server.wait(timeout=10)
    assert rc == 0, f"Server exited with code {rc}"

    channel.close()
