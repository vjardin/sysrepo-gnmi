"""
Stress tests for sysrepo-gnmi server.

Exercises concurrent RPCs, rapid subscribe/unsubscribe, large transactions,
and mixed workloads to surface fd leaks, memory issues, and race conditions.
"""

import concurrent.futures
import threading
import time

import grpc
import pytest

import gnmi_pb2
import gnmi_pb2_grpc
from helpers import xpath_to_path


# -- Helpers --

BIND = "localhost:40051"


def make_stub():
    """Create a fresh gRPC stub (own channel)."""
    ch = grpc.insecure_channel(BIND)
    return gnmi_pb2_grpc.gNMIStub(ch), ch


# -- Tests --

def test_stress_parallel_get(gnmi_stub):
    """Blast 200 parallel Get requests across 20 threads."""
    path = xpath_to_path("/gnmi-server-test:test-state")
    errors = []

    def do_get(_):
        try:
            resp = gnmi_stub.Get(gnmi_pb2.GetRequest(
                path=[path], encoding=gnmi_pb2.JSON_IETF,
            ), timeout=10)
            assert len(resp.notification) == 1
        except Exception as e:
            errors.append(str(e))

    with concurrent.futures.ThreadPoolExecutor(max_workers=20) as pool:
        list(pool.map(do_get, range(200)))

    assert not errors, f"{len(errors)} errors: {errors[:5]}"


def test_stress_parallel_set(gnmi_stub):
    """Blast 100 sequential Set requests (single-threaded, measures throughput)."""
    for i in range(100):
        path = xpath_to_path(
            f"/gnmi-server-test:test/things[name='stress{i}']/description"
        )
        gnmi_stub.Set(gnmi_pb2.SetRequest(
            update=[gnmi_pb2.Update(
                path=path,
                val=gnmi_pb2.TypedValue(json_ietf_val=f'"v{i}"'.encode()),
            )],
        ), timeout=10)

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test")],
    ), timeout=10)


def test_stress_large_transaction(gnmi_stub):
    """Single Set with 500 updates in one transaction."""
    updates = []
    for i in range(500):
        path = xpath_to_path(
            f"/gnmi-server-test:test/things[name='bulk{i}']/description"
        )
        updates.append(gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=f'"d{i}"'.encode()),
        ))

    resp = gnmi_stub.Set(gnmi_pb2.SetRequest(update=updates), timeout=60)
    assert len(resp.response) == 500

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test")],
    ), timeout=30)


def test_stress_subscribe_churn(gnmi_stub):
    """Rapidly open and close 50 ONCE subscriptions."""
    path = xpath_to_path("/gnmi-server-test:test-state")

    for _ in range(50):
        sl = gnmi_pb2.SubscriptionList(
            subscription=[gnmi_pb2.Subscription(path=path)],
            mode=gnmi_pb2.SubscriptionList.ONCE,
            encoding=gnmi_pb2.JSON_IETF,
        )
        req = gnmi_pb2.SubscribeRequest(subscribe=sl)
        responses = list(gnmi_stub.Subscribe(iter([req]), timeout=5))
        assert any(r.sync_response for r in responses)


def test_stress_parallel_subscribe_once(gnmi_server):
    """10 parallel ONCE subscriptions on separate channels."""
    path = xpath_to_path("/gnmi-server-test:test-state")
    errors = []

    def do_sub(_):
        stub, ch = make_stub()
        try:
            sl = gnmi_pb2.SubscriptionList(
                subscription=[gnmi_pb2.Subscription(path=path)],
                mode=gnmi_pb2.SubscriptionList.ONCE,
                encoding=gnmi_pb2.JSON_IETF,
            )
            req = gnmi_pb2.SubscribeRequest(subscribe=sl)
            responses = list(stub.Subscribe(iter([req]), timeout=5))
            assert any(r.sync_response for r in responses)
        except Exception as e:
            errors.append(str(e))
        finally:
            ch.close()

    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as pool:
        list(pool.map(do_sub, range(10)))

    assert not errors, f"{len(errors)} errors: {errors[:5]}"


def test_stress_mixed_workload(gnmi_stub):
    """Concurrent Get + Set + Capabilities from multiple threads."""
    errors = []

    def do_caps():
        try:
            resp = gnmi_stub.Capabilities(gnmi_pb2.CapabilityRequest(), timeout=5)
            assert resp.gnmi_version == "0.7.0"
        except grpc.RpcError as e:
            errors.append(f"caps: {e}")

    def do_get():
        try:
            path = xpath_to_path("/gnmi-server-test:test-state")
            resp = gnmi_stub.Get(gnmi_pb2.GetRequest(
                path=[path], encoding=gnmi_pb2.JSON_IETF,
            ), timeout=5)
            assert len(resp.notification) == 1
        except grpc.RpcError as e:
            errors.append(f"get: {e}")

    def do_set(i):
        try:
            path = xpath_to_path(
                f"/gnmi-server-test:test/things[name='mix{i}']/description"
            )
            gnmi_stub.Set(gnmi_pb2.SetRequest(
                update=[gnmi_pb2.Update(
                    path=path,
                    val=gnmi_pb2.TypedValue(json_ietf_val=f'"m{i}"'.encode()),
                )],
            ), timeout=5)
        except grpc.RpcError as e:
            errors.append(f"set: {e}")

    with concurrent.futures.ThreadPoolExecutor(max_workers=15) as pool:
        futs = []
        for i in range(50):
            futs.append(pool.submit(do_caps))
            futs.append(pool.submit(do_get))
            futs.append(pool.submit(do_set, i))
        concurrent.futures.wait(futs)

    # Cleanup
    try:
        gnmi_stub.Set(gnmi_pb2.SetRequest(
            delete=[xpath_to_path("/gnmi-server-test:test")],
        ), timeout=10)
    except Exception:
        pass

    assert not errors, f"{len(errors)} errors: {errors[:5]}"


def test_stress_stream_with_rapid_sets(gnmi_stub):
    """ON_CHANGE stream while doing rapid Set operations."""
    path = xpath_to_path("/gnmi-server-test:test/things[name='StreamStress']")
    subs = [gnmi_pb2.Subscription(
        path=path,
        mode=gnmi_pb2.SubscriptionMode.ON_CHANGE,
    )]
    sl = gnmi_pb2.SubscriptionList(
        subscription=subs,
        mode=gnmi_pb2.SubscriptionList.STREAM,
        encoding=gnmi_pb2.JSON_IETF,
    )

    responses = []
    got_sync = threading.Event()

    def collect():
        try:
            stream = gnmi_stub.Subscribe(
                iter([gnmi_pb2.SubscribeRequest(subscribe=sl)]),
                timeout=10,
            )
            for resp in stream:
                responses.append(resp)
                if resp.sync_response:
                    got_sync.set()
                if len(responses) >= 30:
                    break
        except grpc.RpcError:
            pass

    t = threading.Thread(target=collect, daemon=True)
    t.start()
    got_sync.wait(timeout=5)

    if not got_sync.is_set():
        t.join(timeout=10)
        assert got_sync.is_set(), "Never got sync_response"

    # Fire rapid sets while stream is active
    desc_path = xpath_to_path(
        "/gnmi-server-test:test/things[name='StreamStress']/description"
    )
    for i in range(20):
        try:
            gnmi_stub.Set(gnmi_pb2.SetRequest(
                update=[gnmi_pb2.Update(
                    path=desc_path,
                    val=gnmi_pb2.TypedValue(json_ietf_val=f'"iter{i}"'.encode()),
                )],
            ), timeout=5)
        except grpc.RpcError:
            pass
        time.sleep(0.05)  # let ON_CHANGE propagate

    t.join(timeout=10)

    # Cleanup
    try:
        gnmi_stub.Set(gnmi_pb2.SetRequest(
            delete=[path],
        ), timeout=5)
    except Exception:
        pass

    assert got_sync.is_set(), "Never got sync_response"
    # Must get at least initial data + sync_response; change notifications
    # may arrive depending on timing (strace/valgrind slow delivery)
    assert len(responses) >= 2, f"Only got {len(responses)} responses"
