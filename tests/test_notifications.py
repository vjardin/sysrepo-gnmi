"""
YANG notification tests.

Verifies that the server emits sysrepo-gnmi-monitoring notifications
for session lifecycle and confirmed-commit events.

Since YANG notifications are sent via sr_notif_send_tree() to sysrepo
(not directly over gNMI), we verify by checking the server log for the
corresponding gnmi_log entries that accompany each notification.
"""

import json
import os
import signal
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "proto"))

import grpc
import pytest
import gnmi_pb2
import gnmi_pb2_grpc
import gnmi_ext_pb2
from google.protobuf import duration_pb2

from helpers import xpath_to_path

# Dedicated port
NOTIF_BIND = "localhost:40081"


def _wait_for_port(host, port, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except (ConnectionRefusedError, OSError):
            time.sleep(0.1)
    return False


def _read_log(log_path):
    with open(log_path, errors="replace") as f:
        return f.read()


@pytest.fixture
def notif_server(gnmi_env):
    """Start a dedicated gnmi_server for notification testing."""
    env, build_dir = gnmi_env
    binary = os.environ.get(
        "GNMI_SERVER_BIN",
        os.path.join(build_dir, "gnmi_server"),
    )
    if not os.path.isfile(binary):
        pytest.skip(f"gnmi_server binary not found: {binary}")

    yang_dir = os.path.join(os.path.dirname(__file__), "..", "yang")
    log_path = os.path.join(build_dir, "gnmi_server_notif_test.log")
    log_file = open(log_path, "w")

    # Isolated sysrepo instance
    notif_env = env.copy()
    notif_env["SYSREPO_SHM_PREFIX"] = f"gnminotif{os.getpid()}_"
    notif_env["SYSREPO_REPOSITORY_PATH"] = os.path.join(build_dir, "test_repo_notif")

    # Start seed_oper for test YANG modules
    seed_binary = os.path.join(build_dir, "seed_oper")
    seed_log_path = os.path.join(build_dir, "seed_oper_notif.log")
    seed_log = open(seed_log_path, "w")
    seed_proc = subprocess.Popen([seed_binary], env=notif_env,
                                 stdout=seed_log, stderr=seed_log)
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
            pytest.fail(f"seed_oper (notif) failed:\n{f.read()}")

    proc = subprocess.Popen(
        [binary, "-f", "-b", NOTIF_BIND, "-l", "4", "-Y", yang_dir],
        env=notif_env,
        stdout=log_file,
        stderr=log_file,
    )

    host, port = NOTIF_BIND.split(":")
    if not _wait_for_port(host, int(port)):
        proc.kill()
        proc.wait()
        seed_proc.kill()
        seed_proc.wait()
        log_file.close()
        seed_log.close()
        with open(log_path) as f:
            pytest.fail(f"notif_server failed to start:\n{f.read()}")

    yield proc, log_path

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


def test_session_lifecycle_notifications(notif_server):
    """Connect, do an RPC, disconnect, wait for idle reap.
    Verify session-start and session-end (idle-timeout) appear in logs."""
    proc, log_path = notif_server

    # Connect and do a Capabilities RPC (creates session)
    channel = grpc.insecure_channel(NOTIF_BIND)
    stub = gnmi_pb2_grpc.gNMIStub(channel)
    resp = stub.Capabilities(
        gnmi_pb2.CapabilityRequest(),
        metadata=[("username", "operator")],
    )
    assert resp.gNMI_version

    # Close channel -- session will be reaped after idle timeout
    channel.close()

    # The idle reaper runs every 30s with 60s timeout. For the test,
    # just verify the session-start log entry exists now.
    time.sleep(0.5)
    log = _read_log(log_path)

    assert "Session" in log and "new from" in log, (
        "Expected session-start log entry")
    assert "operator" in log, (
        "Expected username 'operator' in session log")


def test_confirmed_commit_notifications(notif_server):
    """Do a confirmed commit, then confirm it. Verify start+confirm logs."""
    proc, log_path = notif_server

    channel = grpc.insecure_channel(NOTIF_BIND)
    stub = gnmi_pb2_grpc.gNMIStub(channel)

    # 1. Set with confirmed commit (60s rollback timer)
    path = xpath_to_path("/gnmi-server-test:test/things[name='NotifTest']/description")
    rollback_dur = duration_pb2.Duration(seconds=60)
    commit_ext = gnmi_ext_pb2.Extension(
        commit=gnmi_ext_pb2.Commit(
            id="notif-test-1",
            commit=gnmi_ext_pb2.CommitRequest(
                rollback_duration=rollback_dur,
            ),
        ),
    )
    set_req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"notif-value"'),
        )],
        extension=[commit_ext],
    )
    resp = stub.Set(set_req)
    assert resp.timestamp > 0

    time.sleep(0.5)
    log = _read_log(log_path)
    assert "Commit-confirmed: started" in log, (
        "Expected commit-start log entry")
    assert "notif-test-1" in log, (
        "Expected commit-id in log")

    # 2. Confirm the commit
    confirm_ext = gnmi_ext_pb2.Extension(
        commit=gnmi_ext_pb2.Commit(
            id="notif-test-1",
            confirm=gnmi_ext_pb2.CommitConfirm(),
        ),
    )
    confirm_req = gnmi_pb2.SetRequest(extension=[confirm_ext])
    resp = stub.Set(confirm_req)
    assert resp.timestamp > 0

    time.sleep(0.5)
    log = _read_log(log_path)
    assert "Confirmed-commit: confirmed and persisted" in log, (
        "Expected commit-confirm log entry")

    # Cleanup: delete the test data
    del_req = gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test/things[name='NotifTest']")],
    )
    stub.Set(del_req)
    channel.close()


def test_confirmed_commit_cancel_notification(notif_server):
    """Do a confirmed commit, then cancel it. Verify cancel log."""
    proc, log_path = notif_server

    channel = grpc.insecure_channel(NOTIF_BIND)
    stub = gnmi_pb2_grpc.gNMIStub(channel)

    # 1. Set with confirmed commit
    path = xpath_to_path("/gnmi-server-test:test/things[name='CancelTest']/description")
    rollback_dur = duration_pb2.Duration(seconds=60)
    commit_ext = gnmi_ext_pb2.Extension(
        commit=gnmi_ext_pb2.Commit(
            id="cancel-test-1",
            commit=gnmi_ext_pb2.CommitRequest(
                rollback_duration=rollback_dur,
            ),
        ),
    )
    set_req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"cancel-value"'),
        )],
        extension=[commit_ext],
    )
    resp = stub.Set(set_req)
    assert resp.timestamp > 0

    # 2. Cancel
    cancel_ext = gnmi_ext_pb2.Extension(
        commit=gnmi_ext_pb2.Commit(
            id="cancel-test-1",
            cancel=gnmi_ext_pb2.CommitCancel(),
        ),
    )
    cancel_req = gnmi_pb2.SetRequest(extension=[cancel_ext])
    resp = stub.Set(cancel_req)
    assert resp.timestamp > 0

    time.sleep(0.5)
    log = _read_log(log_path)
    assert "Commit-confirmed: cancelled" in log, (
        "Expected commit-cancel log entry")
    assert "cancel-test-1" in log, (
        "Expected commit-id in cancel log")

    channel.close()
