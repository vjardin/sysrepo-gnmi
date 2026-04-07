"""
pytest fixtures for sysrepo-gnmi conformance tests.

Starts gnmi_server as a subprocess, creates gRPC channel + stub,
and provides helpers for sysrepo state manipulation.

Compliance reporting: tests are auto-classified into gNMI/RFC spec
sections for JUnit XML grouping and compliance matrix output.
"""

import os
import sys
import time
import signal
import socket
import subprocess
from collections import OrderedDict

import grpc
import pytest


# gNMI / RFC spec-section compliance classification
# Order matters: first match wins, so more specific prefixes come first.
# Section numbers reference the gNMI specification (OpenConfig) and RFCs.

_COMPLIANCE_MAP = [
    # --- gNMI §2.2.2 Paths (structured path encoding)
    ("test_get_name_with_", "gNMI §2.2.2 Paths - special chars"),
    ("test_get_composite_key", "gNMI §2.2.2 Paths - composite keys"),
    ("test_get_leaf_parent_with_", "gNMI §2.2.2 Paths - special chars"),
    ("test_get_relative_path", "gNMI §2.2.2 Paths - validation"),
    ("test_subscribe_on_change_composite_key", "gNMI §2.2.2 Paths - composite keys"),

    # --- gNMI §2.2.2.1 Path Target
    ("test_get_with_target", "gNMI §2.2.2.1 Path Target"),
    ("test_subscribe_once_with_target", "gNMI §2.2.2.1 Path Target"),

    # --- gNMI §2.2.3 TypedValue (node values)
    ("test_set_empty_leaf", "gNMI §2.2.3 TypedValue"),
    ("test_set_boolean_leaf", "gNMI §2.2.3 TypedValue"),
    ("test_set_integer_leaf", "gNMI §2.2.3 TypedValue"),
    ("test_set_decimal_leaf", "gNMI §2.2.3 TypedValue"),
    ("test_set_decimal64_val_type", "gNMI §2.2.3 TypedValue"),
    ("test_set_any_val_type", "gNMI §2.2.3 TypedValue"),
    ("test_set_leaflist_val_type", "gNMI §2.2.3 TypedValue"),

    # --- gNMI §2.3 Encoding
    ("test_get_ascii", "gNMI §2.3.4 Encoding ASCII"),
    ("test_get_proto", "gNMI §2.3.3 Encoding PROTO"),
    ("test_get_unsupported_encoding", "gNMI §2.3 Encoding - error"),
    ("test_set_json_val_type", "gNMI §2.3.1 Encoding JSON"),
    ("test_set_proto_bytes_val_type", "gNMI §2.3.3 Encoding PROTO"),
    ("test_subscribe_once_proto", "gNMI §2.3.3 Encoding PROTO"),
    ("test_subscribe_unsupported_encoding", "gNMI §2.3 Encoding - error"),

    # --- gNMI §2.4.1 Path Prefixes
    ("test_get_with_prefix", "gNMI §2.4.1 Path Prefixes"),
    ("test_set_with_prefix", "gNMI §2.4.1 Path Prefixes"),
    ("test_set_with_empty_prefix", "gNMI §2.4.1 Path Prefixes"),
    ("test_set_incorrect_prefix", "gNMI §2.4.1 Path Prefixes"),
    ("test_subscribe_once_with_prefix", "gNMI §2.4.1 Path Prefixes"),

    # --- gNMI §2.6 use_models
    ("test_get_use_models", "gNMI §2.6 use_models"),

    # --- gNMI §2.7 Origin
    ("test_get_empty_origin", "gNMI §2.7 Origin"),
    ("test_get_no_origin", "gNMI §2.7 Origin"),
    ("test_get_wrong_origin", "gNMI §2.7 Origin"),

    # --- gNMI §3.1 Per-RPC auth (metadata username)
    ("test_get_with_grpc_metadata", "gNMI §3.1 Per-RPC Auth"),

    # --- gNMI §3.2 Capabilities
    ("test_capability", "gNMI §3.2 Capabilities"),
    ("test_gnmic_capabilities", "gNMI §3.2 Capabilities"),

    # --- gNMI §3.3 Get
    ("test_get_state_datatype", "gNMI §3.3.1 Get - DataType STATE"),
    ("test_get_config_datastore", "gNMI §3.3.1 Get - DataType CONFIG"),
    ("test_get_multiple_paths", "gNMI §3.3.1 Get - multiple paths"),
    ("test_get_wildcard", "gNMI §3.3.1 Get - wildcards"),
    ("test_get_list_container", "gNMI §3.3.1 Get - wildcards"),
    ("test_get_nonexistent_path", "gNMI §3.3.4 Get - error handling"),
    ("test_get_invalid_datatype", "gNMI §3.3.4 Get - error handling"),
    ("test_get_empty_container", "gNMI §3.3.4 Get - error handling"),
    ("test_gnmic_get_depth", "gNMI ext.Depth"),
    ("test_gnmic_get_no_depth", "gNMI ext.Depth"),
    ("test_gnmic_get", "gNMI §3.3 Get"),
    ("test_get", "gNMI §3.3 Get"),

    # --- gNMI §3.4 Set
    ("test_set_validate", "gNMI §3.4 Set - validate-only"),
    ("test_set_candidate", "RFC 8342 §4.2 Candidate DS"),
    ("test_set_while_waiting_confirm", "gNMI ext.Commit"),
    ("test_set_replace_removes_absent", "gNMI §3.4.4 Set - replace semantics"),
    ("test_set_replace_empty", "gNMI §3.4.4 Set - replace"),
    ("test_set_top_level_replace", "gNMI §3.4.4 Set - replace"),
    ("test_set_leaflist_replace", "gNMI §3.4.4 Set - replace"),
    ("test_set_leaflist_inner_replace", "gNMI §3.4.4 Set - replace"),
    ("test_set_multi_replace", "gNMI §3.4.4 Set - replace"),
    ("test_set_delete", "gNMI §3.4.6 Set - delete"),
    ("test_set_top_level_delete", "gNMI §3.4.6 Set - delete"),
    ("test_set_composite_key", "gNMI §3.4.5 Set - composite keys"),
    ("test_set_wine_composite_key", "gNMI §3.4.5 Set - composite keys"),
    ("test_set_transaction", "gNMI §3.4.3 Set - transactions"),
    ("test_set_scaled", "gNMI §3.4.3 Set - transactions"),
    ("test_set_failing", "gNMI §3.4.7 Set - error handling"),
    ("test_set_application_error", "gNMI §3.4.7 Set - error handling"),
    ("test_set_data_model_error", "gNMI §3.4.7 Set - error handling"),
    ("test_set_no_val_type", "gNMI §3.4.7 Set - error handling"),
    ("test_set_no_path", "gNMI §3.4.7 Set - error handling"),
    ("test_set_no_namespace", "gNMI §3.4.7 Set - error handling"),
    ("test_set_unsupported_val_types", "gNMI §3.4.7 Set - error handling"),
    ("test_set_more_unsupported_val_types", "gNMI §3.4.7 Set - error handling"),
    ("test_set_wildcard_in_update", "gNMI §3.4.6 Set - delete"),
    ("test_gnmic_set", "gNMI §3.4 Set"),
    ("test_set", "gNMI §3.4.4 Set - update"),

    # --- gNMI §3.5 Subscribe
    ("test_subscribe_sample_suppress", "gNMI §3.5.1.5.2 suppress_redundant"),
    ("test_subscribe_on_change_heartbeat", "gNMI §3.5.1.5.2 heartbeat"),
    ("test_subscribe_allow_aggregation", "gNMI §3.5.1.2 allow_aggregation"),
    ("test_subscribe_once_updates_only", "gNMI §3.5.1.2 updates_only"),
    ("test_subscribe_stream_updates_only", "gNMI §3.5.1.2 updates_only"),
    ("test_subscribe_poll_updates_only", "gNMI §3.5.1.2 updates_only"),
    ("test_subscribe_too_many", "gNMI §3.5.1.2 SubscriptionList limits"),
    ("test_subscribe_at_max", "gNMI §3.5.1.2 SubscriptionList limits"),
    ("test_subscribe_empty", "gNMI §3.5.1.1 SubscribeRequest"),
    ("test_subscribe_once_invalid_mode", "gNMI §3.5.1.1 SubscribeRequest"),
    ("test_subscribe_poll_dup_sub", "gNMI §3.5.1.1 SubscribeRequest"),
    ("test_subscribe_poll_alias", "gNMI §3.5 Subscribe - aliases"),
    ("test_subscribe_poll_use_aliases", "gNMI §3.5 Subscribe - aliases"),
    ("test_subscribe_stream_use_aliases", "gNMI §3.5 Subscribe - aliases"),
    ("test_subscribe_once", "gNMI §3.5.1.5.1 Subscribe ONCE"),
    ("test_subscribe_poll", "gNMI §3.5.1.5.3 Subscribe POLL"),
    ("test_subscribe_stream", "gNMI §3.5.1.5.2 Subscribe STREAM"),
    ("test_subscribe_on_change", "gNMI §3.5.1.5.2 STREAM ON_CHANGE"),
    ("test_subscribe_sample", "gNMI §3.5.1.5.2 STREAM SAMPLE"),
    ("test_subscribe_for_leaf", "gNMI §3.5.1.5.2 STREAM ON_CHANGE"),
    ("test_gnmic_subscribe", "gNMI §3.5 Subscribe"),
    ("test_subscribe", "gNMI §3.5 Subscribe"),

    # --- gNMI ext.Commit (confirmed commit with rollback)
    ("test_confirm", "gNMI ext.Commit"),
    ("test_commit_confirmed", "gNMI ext.Commit"),

    # --- RFC 8341 NACM (per-operation access control)
    ("test_nacm_read", "RFC 8341 §3.4.1 NACM read"),
    ("test_nacm_write", "RFC 8341 §3.4.2 NACM write"),
    ("test_nacm_exec", "RFC 8341 §3.4.5 NACM exec"),
    ("test_nacm_capabilities", "RFC 8341 §3.7 NACM defaults"),

    # --- gNMI RPC/Action invocation
    ("test_get_schema", "RFC 6022 get-schema"),
    ("test_rpc", "gNMI RPC/Action"),

    # --- Stress / performance
    ("test_stress", "Stress Tests"),

    # --- Monitoring & Notifications ---
    ("test_monitoring", "Operational Monitoring"),
    ("test_session_lifecycle_notifications", "YANG Notifications - session"),
    ("test_confirmed_commit_notifications", "YANG Notifications - commit"),
    ("test_confirmed_commit_cancel", "YANG Notifications - commit"),
    ("test_kill_session", "YANG Notifications - kill-session"),

    # --- Graceful shutdown ---
    ("test_graceful_shutdown", "Graceful Shutdown"),

    # --- gnmic interop (catch-all for remaining gnmic tests) ---
    ("test_gnmic", "Interop gnmic"),
]

# Preferred display order for the compliance matrix (spec section order)
_SECTION_ORDER = [
    # Section 2: Common types & encodings
    "gNMI §2.2.2 Paths - special chars",
    "gNMI §2.2.2 Paths - composite keys",
    "gNMI §2.2.2 Paths - validation",
    "gNMI §2.2.2.1 Path Target",
    "gNMI §2.2.3 TypedValue",
    "gNMI §2.3.1 Encoding JSON",
    "gNMI §2.3.3 Encoding PROTO",
    "gNMI §2.3.4 Encoding ASCII",
    "gNMI §2.3 Encoding - error",
    "gNMI §2.4.1 Path Prefixes",
    "gNMI §2.6 use_models",
    "gNMI §2.7 Origin",
    # Section 3: RPCs
    "gNMI §3.1 Per-RPC Auth",
    "gNMI §3.2 Capabilities",
    "gNMI §3.3 Get",
    "gNMI §3.3.1 Get - DataType CONFIG",
    "gNMI §3.3.1 Get - DataType STATE",
    "gNMI §3.3.1 Get - multiple paths",
    "gNMI §3.3.1 Get - wildcards",
    "gNMI §3.3.4 Get - error handling",
    "gNMI §3.4.3 Set - transactions",
    "gNMI §3.4.4 Set - update",
    "gNMI §3.4.4 Set - replace",
    "gNMI §3.4.4 Set - replace semantics",
    "gNMI §3.4.5 Set - composite keys",
    "gNMI §3.4.6 Set - delete",
    "gNMI §3.4.7 Set - error handling",
    "gNMI §3.4 Set",
    "gNMI §3.5.1.1 SubscribeRequest",
    "gNMI §3.5.1.2 SubscriptionList limits",
    "gNMI §3.5.1.2 updates_only",
    "gNMI §3.5.1.2 allow_aggregation",
    "gNMI §3.5.1.5.1 Subscribe ONCE",
    "gNMI §3.5.1.5.2 Subscribe STREAM",
    "gNMI §3.5.1.5.2 STREAM ON_CHANGE",
    "gNMI §3.5.1.5.2 STREAM SAMPLE",
    "gNMI §3.5.1.5.2 suppress_redundant",
    "gNMI §3.5.1.5.2 heartbeat",
    "gNMI §3.5.1.5.3 Subscribe POLL",
    "gNMI §3.5 Subscribe - aliases",
    "gNMI §3.5 Subscribe",
    # Extensions
    "gNMI ext.Commit",
    "gNMI ext.Depth",
    # RFCs
    "RFC 8341 §3.4.1 NACM read",
    "RFC 8341 §3.4.2 NACM write",
    "RFC 8341 §3.4.5 NACM exec",
    "RFC 8341 §3.7 NACM defaults",
    "RFC 8342 §4.2 Candidate DS",
    # Other
    "gNMI RPC/Action",
    "Stress Tests",
    "Operational Monitoring",
    "YANG Notifications - session",
    "YANG Notifications - commit",
    "Graceful Shutdown",
    "Interop gnmic",
]


def _classify_test(name):
    """Return the gNMI/RFC spec section for a test function name."""
    for prefix, section in _COMPLIANCE_MAP:
        if name.startswith(prefix):
            return section
    return "Uncategorized"


# Collect per-test results for the compliance matrix
_compliance_results = OrderedDict()


@pytest.fixture(autouse=True)
def _compliance_classname(request, record_xml_attribute):
    """Override JUnit XML classname to group tests by spec section."""
    section = _classify_test(request.node.name)
    record_xml_attribute("classname", section)


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    """Collect test outcomes by spec section for compliance matrix."""
    outcome = yield
    report = outcome.get_result()
    if call.when == "call":
        section = _classify_test(item.name)
        _compliance_results.setdefault(section, [])
        _compliance_results[section].append((item.name, report.outcome))
    elif call.when == "setup" and report.skipped:
        section = _classify_test(item.name)
        _compliance_results.setdefault(section, [])
        _compliance_results[section].append((item.name, "skipped"))


def pytest_terminal_summary(terminalreporter, exitstatus, config):
    """Print gNMI compliance matrix and write Markdown summary."""
    if not _compliance_results:
        return

    # Build rows in preferred display order
    rows = []
    seen = set()
    for section in _SECTION_ORDER:
        if section in _compliance_results:
            seen.add(section)
            tests = _compliance_results[section]
            passed = sum(1 for _, o in tests if o == "passed")
            failed = sum(1 for _, o in tests if o == "failed")
            skipped = sum(1 for _, o in tests if o == "skipped")
            rows.append((section, len(tests), passed, failed, skipped))
    # Append any sections not in the preferred order
    for section, tests in _compliance_results.items():
        if section not in seen:
            passed = sum(1 for _, o in tests if o == "passed")
            failed = sum(1 for _, o in tests if o == "failed")
            skipped = sum(1 for _, o in tests if o == "skipped")
            rows.append((section, len(tests), passed, failed, skipped))

    # Terminal output
    terminalreporter.section("gNMI Compliance Matrix")
    hdr = f"{'Spec Section':<40} {'Total':>5} {'Pass':>5} {'Fail':>5} {'Skip':>5}  Status"
    terminalreporter.write_line(hdr)
    terminalreporter.write_line("-" * len(hdr))
    for section, total, passed, failed, skipped in rows:
        if failed > 0:
            status = "FAIL"
        elif skipped > 0 and passed > 0:
            status = "PARTIAL"
        elif skipped > 0:
            status = "SKIP"
        else:
            status = "PASS"
        terminalreporter.write_line(
            f"{section:<40} {total:>5} {passed:>5} {failed:>5} {skipped:>5}  {status}"
        )
    total_tests = sum(t for _, t, _, _, _ in rows)
    total_pass = sum(p for _, _, p, _, _ in rows)
    total_fail = sum(f for _, _, _, f, _ in rows)
    total_skip = sum(s for _, _, _, _, s in rows)
    terminalreporter.write_line("-" * len(hdr))
    terminalreporter.write_line(
        f"{'TOTAL':<40} {total_tests:>5} {total_pass:>5} {total_fail:>5} {total_skip:>5}"
    )

    # Write Markdown summary for CI ($GITHUB_STEP_SUMMARY)
    build_dir = os.environ.get("GNMI_BUILD_DIR", "builddir")
    try:
        summary_path = os.path.join(build_dir, "compliance-summary.md")
        with open(summary_path, "w") as f:
            f.write("## gNMI Compliance Matrix\n\n")
            f.write("| Spec Section | Total | Pass | Fail | Skip | Status |\n")
            f.write("|---|:---:|:---:|:---:|:---:|:---:|\n")
            for section, total, passed, failed, skipped in rows:
                if failed > 0:
                    status = ":x:"
                elif skipped > 0 and passed > 0:
                    status = ":warning:"
                elif skipped > 0:
                    status = ":fast_forward:"
                else:
                    status = ":white_check_mark:"
                f.write(f"| {section} | {total} | {passed} "
                        f"| {failed} | {skipped} | {status} |\n")
            f.write(f"| **TOTAL** | **{total_tests}** | **{total_pass}** "
                    f"| **{total_fail}** | **{total_skip}** | |\n")
    except OSError:
        pass  # Non-fatal: CI directory may not exist in local runs

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
    # Per-subsystem debug logging for CI and local test runs
    env.setdefault("GNMI_LOG_LEVEL_SR", "4")
    env.setdefault("GNMI_LOG_LEVEL_LY", "4")
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

    # Locate server YANG modules directory (yang/ in source root)
    yang_dir = os.path.join(os.path.dirname(__file__), "..", "yang")

    # Optionally run under valgrind or strace
    cmd = [binary, "-f", "-b", GNMI_BIND, "-l", "4", "-Y", yang_dir]
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
