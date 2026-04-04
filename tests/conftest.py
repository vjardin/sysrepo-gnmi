"""
pytest fixtures for sysrepo-gnmi conformance tests.

Starts gnmi_server as a subprocess, creates gRPC channel + stub,
and provides helpers for sysrepo state manipulation.
"""

import os
import sys
import time
import signal
import socket
import subprocess

import grpc
import pytest

# Add tests/ dir and proto/ subdir to path for imports
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "proto"))

import gnmi_pb2
import gnmi_pb2_grpc

GNMI_BIND = "localhost:40051"
GNMI_TIMEOUT = 10  # seconds


def _wait_for_port(host, port, timeout=10):
    """Block until TCP port is accepting connections."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((host, port), timeout=0.5):
                return True
        except (ConnectionRefusedError, OSError):
            time.sleep(0.1)
    return False


@pytest.fixture(scope="session")
def gnmi_env():
    """Environment variables for test isolation."""
    build_dir = os.environ.get(
        "GNMI_BUILD_DIR",
        os.path.join(os.path.dirname(__file__), "..", "builddir"),
    )
    deps_lib = os.path.join(build_dir, "deps", "lib")

    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = deps_lib + ":" + env.get("LD_LIBRARY_PATH", "")
    # Use unique SHM prefix to avoid conflicts with stale test runs
    shm_prefix = env.get("SYSREPO_SHM_PREFIX", f"gnmitest{os.getpid()}_")
    env["SYSREPO_SHM_PREFIX"] = shm_prefix
    repo_path = env.get("SYSREPO_REPOSITORY_PATH",
                        os.path.join(build_dir, "test_repo"))
    env["SYSREPO_REPOSITORY_PATH"] = repo_path
    env["GNMI_YANG_DIR"] = os.environ.get(
        "GNMI_YANG_DIR",
        os.path.join(os.path.dirname(__file__), "yang"),
    )
    return env, build_dir


@pytest.fixture(scope="session")
def seed_oper(gnmi_env):
    """Start seed_oper to provide operational test data via sysrepo."""
    env, build_dir = gnmi_env
    binary = os.path.join(build_dir, "seed_oper")
    if not os.path.isfile(binary):
        pytest.skip(f"seed_oper binary not found: {binary}")

    log_path = os.path.join(build_dir, "seed_oper.log")
    log_file = open(log_path, "w")
    proc = subprocess.Popen(
        [binary],
        env=env,
        stdout=log_file,
        stderr=log_file,
    )
    # Wait for seed_oper to report readiness (polls log file)
    deadline = time.monotonic() + 30
    ready = False
    while time.monotonic() < deadline:
        log_file.flush()
        if proc.poll() is not None:
            break
        try:
            with open(log_path, "r") as check:
                if "operational data provider ready" in check.read():
                    ready = True
                    break
        except FileNotFoundError:
            pass
        time.sleep(0.2)
    if not ready:
        if proc.poll() is not None:
            log_file.close()
            with open(log_path) as f:
                pytest.fail(f"seed_oper died (rc={proc.returncode}):\n{f.read()}")
        else:
            proc.kill()
            proc.wait()
            log_file.close()
            with open(log_path) as f:
                pytest.fail(f"seed_oper never became ready:\n{f.read()}")

    yield proc

    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
    log_file.close()


@pytest.fixture(scope="session")
def gnmi_server(gnmi_env, seed_oper):
    """Start gnmi_server as a subprocess for the entire test session."""
    env, build_dir = gnmi_env
    binary = os.environ.get(
        "GNMI_SERVER_BIN",
        os.path.join(build_dir, "gnmi_server"),
    )

    if not os.path.isfile(binary):
        pytest.skip(f"gnmi_server binary not found: {binary}")

    server_log_path = os.path.join(build_dir, "gnmi_server.log")
    server_log = open(server_log_path, "w")

    # Optionally run under valgrind or strace
    cmd = [binary, "-f", "-b", GNMI_BIND, "-l", "4"]
    use_valgrind = os.environ.get("GNMI_VALGRIND", "")
    use_strace = os.environ.get("GNMI_STRACE", "")
    if use_valgrind:
        valgrind_log = os.path.join(build_dir, "valgrind.log")
        cmd = ["valgrind", "--leak-check=full", "--track-origins=yes",
               "--error-exitcode=99",
               f"--log-file={valgrind_log}"] + cmd
    elif use_strace:
        strace_log = os.path.join(build_dir, "strace.log")
        cmd = ["strace", "-f", "-e", "trace=network,desc,memory",
               "-o", strace_log] + cmd

    proc = subprocess.Popen(
        cmd,
        env=env,
        stdout=server_log,
        stderr=server_log,
    )

    host, port = GNMI_BIND.split(":")
    wait_timeout = 60 if (use_valgrind or use_strace) else GNMI_TIMEOUT
    if not _wait_for_port(host, int(port), timeout=wait_timeout):
        proc.kill()
        stdout, stderr = proc.communicate(timeout=5)
        pytest.fail(
            f"gnmi_server failed to start:\n"
            f"stdout: {stdout.decode()}\n"
            f"stderr: {stderr.decode()}"
        )

    yield proc

    proc.send_signal(signal.SIGTERM)
    shutdown_timeout = 30 if use_valgrind else 5
    try:
        proc.wait(timeout=shutdown_timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

    server_log.close()

    if use_valgrind:
        valgrind_log = os.path.join(build_dir, "valgrind.log")
        if os.path.isfile(valgrind_log):
            with open(valgrind_log) as f:
                for line in f:
                    if "ERROR SUMMARY" in line or "definitely lost" in line:
                        print(f"VALGRIND: {line.strip()}")


@pytest.fixture(scope="session")
def grpc_channel(gnmi_server):
    """Create an insecure gRPC channel to the server."""
    # Bypass any HTTP proxy for localhost connections
    os.environ["no_proxy"] = "localhost,127.0.0.1"
    os.environ["NO_PROXY"] = "localhost,127.0.0.1"
    channel = grpc.insecure_channel(GNMI_BIND)
    yield channel
    channel.close()


@pytest.fixture(scope="session")
def gnmi_stub(grpc_channel):
    """Create gNMI service stub."""
    return gnmi_pb2_grpc.gNMIStub(grpc_channel)
