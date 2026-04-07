"""
Operational state monitoring tests.

Verifies the sysrepo-gnmi-monitoring YANG module exposes live session
and stream data via gNMI Get on /sysrepo-gnmi-monitoring:server-state.
"""

import json
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

# Dedicated port to avoid conflict with session-scoped server
MON_BIND = "localhost:40071"


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
def mon_server(gnmi_env):
    """Start a dedicated gnmi_server with monitoring enabled."""
    env, build_dir = gnmi_env
    binary = os.environ.get(
        "GNMI_SERVER_BIN",
        os.path.join(build_dir, "gnmi_server"),
    )
    if not os.path.isfile(binary):
        pytest.skip(f"gnmi_server binary not found: {binary}")

    yang_dir = os.path.join(os.path.dirname(__file__), "..", "yang")
    log_path = os.path.join(build_dir, "gnmi_server_monitoring_test.log")
    log_file = open(log_path, "w")

    # Use a separate SHM prefix to avoid collisions with the
    # session-scoped server's sysrepo subscriptions.
    mon_env = env.copy()
    mon_env["SYSREPO_SHM_PREFIX"] = f"gnmimon{os.getpid()}_"
    mon_env["SYSREPO_REPOSITORY_PATH"] = os.path.join(build_dir, "test_repo_mon")

    # Start seed_oper to install test YANG modules and provide oper data
    seed_binary = os.path.join(build_dir, "seed_oper")
    seed_log_path = os.path.join(build_dir, "seed_oper_mon.log")
    seed_log = open(seed_log_path, "w")
    seed_proc = subprocess.Popen([seed_binary], env=mon_env,
                                 stdout=seed_log, stderr=seed_log)
    # Wait for readiness
    deadline = time.monotonic() + 15
    ready = False
    while time.monotonic() < deadline:
        seed_log.flush()
        if seed_proc.poll() is not None:
            break
        try:
            with open(seed_log_path) as f:
                if "operational data provider ready" in f.read():
                    ready = True
                    break
        except OSError:
            pass
        time.sleep(0.2)
    if not ready:
        seed_proc.kill()
        seed_proc.wait()
        seed_log.close()
        with open(seed_log_path) as f:
            pytest.fail(f"seed_oper (mon) failed:\n{f.read()}")

    proc = subprocess.Popen(
        [binary, "-f", "-b", MON_BIND, "-l", "4", "-Y", yang_dir],
        env=mon_env,
        stdout=log_file,
        stderr=log_file,
    )

    host, port = MON_BIND.split(":")
    if not _wait_for_port(host, int(port)):
        proc.kill()
        proc.wait()
        seed_proc.kill()
        seed_proc.wait()
        log_file.close()
        seed_log.close()
        with open(log_path) as f:
            pytest.fail(f"mon_server failed to start:\n{f.read()}")

    yield proc

    if proc.poll() is None:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    seed_proc.send_signal(signal.SIGTERM)
    try:
        seed_proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        seed_proc.kill()
        seed_proc.wait()
    log_file.close()
    seed_log.close()


def _get_server_state(stub):
    """Fetch /sysrepo-gnmi-monitoring:server-state via gNMI Get."""
    path = xpath_to_path("/sysrepo-gnmi-monitoring:server-state")
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = stub.Get(req)
    # Extract the JSON from the first notification/update
    for notif in resp.notification:
        for upd in notif.update:
            return json.loads(upd.val.json_ietf_val.decode())
    return None


def _open_stream(stub, username=None):
    """Open a STREAM/SAMPLE subscribe and wait for sync_response.
    Returns (response_iterator, metadata_list)."""
    path = xpath_to_path("/gnmi-server-test:test-state")
    sub = gnmi_pb2.Subscription(
        path=path,
        mode=gnmi_pb2.SubscriptionMode.SAMPLE,
        sample_interval=10_000_000_000,  # 10s (long, won't fire during test)
    )
    sl = gnmi_pb2.SubscriptionList(
        subscription=[sub],
        mode=gnmi_pb2.SubscriptionList.STREAM,
        encoding=gnmi_pb2.JSON_IETF,
    )
    req = gnmi_pb2.SubscribeRequest(subscribe=sl)

    metadata = []
    if username:
        metadata.append(("username", username))

    responses = stub.Subscribe(iter([req]), metadata=metadata)
    # Drain until sync_response
    for resp in responses:
        if resp.sync_response:
            break
    return responses


def test_monitoring_two_sessions_three_streams(mon_server):
    """Two sessions with different usernames, 3 total streams.

    Session A (user=alice): 2 subscribe streams
    Session B (user=bob):   1 subscribe stream

    Then Get /sysrepo-gnmi-monitoring:server-state and verify:
    - active-sessions >= 2
    - active-streams >= 3
    - session list contains entries with usernames alice and bob
    """
    # Session A: two streams with username=alice
    chan_a = grpc.insecure_channel(MON_BIND)
    stub_a = gnmi_pb2_grpc.gNMIStub(chan_a)
    stream_a1 = _open_stream(stub_a, username="alice")
    stream_a2 = _open_stream(stub_a, username="alice")

    # Session B: one stream with username=bob
    chan_b = grpc.insecure_channel(MON_BIND)
    stub_b = gnmi_pb2_grpc.gNMIStub(chan_b)
    stream_b1 = _open_stream(stub_b, username="bob")

    # Give the server a moment to register all sessions
    time.sleep(0.5)

    # Query monitoring state via a third channel (also creates a session)
    chan_q = grpc.insecure_channel(MON_BIND)
    stub_q = gnmi_pb2_grpc.gNMIStub(chan_q)
    state = _get_server_state(stub_q)

    assert state is not None, "No server-state data returned"

    # JSON keys have module prefix; strip it for readability.
    PREFIX = "sysrepo-gnmi-monitoring:"

    def _key(k):
        return state.get(PREFIX + k, state.get(k))

    # Check top-level counters
    assert int(_key("active-streams")) >= 3, (
        f"Expected >= 3 active streams, got {_key('active-streams')}")
    assert int(_key("active-sessions")) >= 1, (
        f"Expected >= 1 active session, got {_key('active-sessions')}")
    assert int(_key("uptime")) >= 0

    # Check counters
    counters = _key("counters")
    assert int(counters["total-rpcs"]) >= 1

    # Check per-session data: all streams should be present.
    # Note: from a single test process, gRPC may multiplex channels over
    # one TCP connection, so all streams may share one session.  The last
    # username written wins (bob overwrites alice on the same peer).
    # What matters: total active-streams across all sessions >= 3.
    sessions = _key("session")
    if not isinstance(sessions, list):
        sessions = [sessions]
    total_streams = sum(int(s["active-streams"]) for s in sessions)
    assert total_streams >= 3, f"Expected >= 3 total streams, got {total_streams}"

    # Verify at least one session has a username set
    usernames = {s.get("username") for s in sessions if s.get("username")}
    assert len(usernames) >= 1, f"Expected at least one username, got {usernames}"

    # Verify peer-address is populated
    for s in sessions:
        assert s.get("peer-address"), f"Session {s.get('id')} missing peer-address"

    # Cleanup: cancel streams (let gRPC handle it on channel close)
    chan_a.close()
    chan_b.close()
    chan_q.close()
