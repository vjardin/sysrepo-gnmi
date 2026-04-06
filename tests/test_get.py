"""
Get RPC conformance tests.
"""

import json
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "proto"))

import grpc
import pytest
import gnmi_pb2
import gnmi_pb2_grpc

from helpers import xpath_to_path, path_to_xpath


# - Positive tests ---------------------------------------------------


def test_get_top_level(gnmi_stub):
    """Top-level Get request: root path '/' returns data."""
    req = gnmi_pb2.GetRequest(
        path=[gnmi_pb2.Path(origin="rfc7951")],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    assert resp.notification[0].timestamp > 0


def test_get_oper_state(gnmi_stub):
    """Get request of all module oper state.

    Equivalent to: "Get request of all module oper state" [get]
    """
    path = xpath_to_path("/gnmi-server-test:test-state")
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    notif = resp.notification[0]
    assert notif.timestamp > 0
    # Should have updates for the test-state data
    assert notif.update  # at least one update


def test_get_one_list_item_oper(gnmi_stub):
    """Get request of one list item oper state.

    Equivalent to: "Get request of one list item oper state" [get]
    """
    path = xpath_to_path("/gnmi-server-test:test-state/things[name='A']")
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    notif = resp.notification[0]
    assert len(notif.update) == 1
    upd = notif.update[0]
    val = upd.val.json_ietf_val.decode()
    data = json.loads(val)
    assert data.get("name") == "A" or "A" in val


def test_get_one_leaf_oper(gnmi_stub):
    """Get request of one leaf of oper state.

    Equivalent to: "Get request of one leaf of oper state" [get]
    """
    path = xpath_to_path(
        "/gnmi-server-test:test-state/things[name='A']/counter"
    )
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    notif = resp.notification[0]
    assert len(notif.update) == 1
    upd = notif.update[0]
    val = upd.val.json_ietf_val.decode()
    # Leaf value: counter should be "1" (uint64 as quoted string in JSON)
    assert "1" in val


def test_get_with_prefix(gnmi_stub):
    """Get request with prefix.

    Equivalent to: "Get request with prefix" [get]
    """
    prefix = xpath_to_path("/gnmi-server-test:test-state")
    path = xpath_to_path("/things[name='A']")
    # Path should not have origin when prefix has it
    path.origin = ""
    req = gnmi_pb2.GetRequest(
        prefix=prefix,
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    assert len(resp.notification[0].update) >= 1


def test_get_with_target(gnmi_stub):
    """Get request with target.

    Equivalent to: "Get request with target" [get]
    """
    # prefix has target but no elems -> origin must be on path
    prefix = gnmi_pb2.Path(target="mydevice")
    path = xpath_to_path("/gnmi-server-test:test-state/things[name='A']")
    req = gnmi_pb2.GetRequest(
        prefix=prefix,
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    assert resp.notification[0].prefix.target == "mydevice"


def test_get_config_datastore(gnmi_stub):
    """Get request for config datastore.

    Equivalent to: "Get request for config datastore" [get]
    """
    path = xpath_to_path("/gnmi-server-test:test")
    req = gnmi_pb2.GetRequest(
        path=[path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    # Empty config should return no updates (or empty)


def test_get_multiple_paths(gnmi_stub):
    """Get request with multiple paths.

    Equivalent to: "Get request with multiple paths" [get]
    """
    path1 = xpath_to_path("/gnmi-server-test:test-state/things[name='A']")
    path2 = xpath_to_path("/gnmi-server-test:test-state/things[name='B']")
    req = gnmi_pb2.GetRequest(
        path=[path1, path2],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    # Each path should produce its own notification
    assert len(resp.notification) == 2


def test_get_nonexistent_path(gnmi_stub):
    """Get request with non-existent path.

    Equivalent to: "Get request with non-existent path" [get]
    """
    path = xpath_to_path("/gnmi-server-test:nonexistent")
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    assert len(resp.notification[0].update) == 0


def test_get_empty_container(gnmi_stub):
    """Get request of empty container.

    Equivalent to: "Get request of empty container" [get]
    """
    path = xpath_to_path("/gnmi-server-test:test-state/cargo")
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    if resp.notification[0].update:
        val = resp.notification[0].update[0].val.json_ietf_val.decode()
        assert val == "{}"


# - Negative tests ---------------------------------------------------


def test_get_wildcard(gnmi_stub):
    """Get request with wildcard path.

    Equivalent to: "Get request with wildcard" [get]
    """
    path = xpath_to_path("/gnmi-server-test:test-state/things/counter")
    # Use wildcard via direct elem construction
    path_wc = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test-state"),
        gnmi_pb2.PathElem(name="*"),
        gnmi_pb2.PathElem(name="counter"),
    ])
    req = gnmi_pb2.GetRequest(
        path=[path_wc],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    # Wildcard should match things[name='A']/counter and things[name='B']/counter
    assert len(resp.notification[0].update) >= 2


def test_get_list_container(gnmi_stub):
    """Get request of list container.

    Equivalent to: "Get request of list container" [get]
    """
    path = xpath_to_path("/gnmi-server-test:test-state/things")
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    # Should have updates for list entries A and B
    assert len(resp.notification[0].update) >= 2


def _create_thing(gnmi_stub, name):
    """Helper: create a list entry in config with given name."""
    path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test"),
        gnmi_pb2.PathElem(name="things", key={"name": name}),
        gnmi_pb2.PathElem(name="description"),
    ])
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"test_val"'),
        )],
    )
    gnmi_stub.Set(req, timeout=5)


def _delete_thing(gnmi_stub, name):
    """Helper: delete a list entry."""
    path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test"),
        gnmi_pb2.PathElem(name="things", key={"name": name}),
    ])
    try:
        gnmi_stub.Set(gnmi_pb2.SetRequest(delete=[path]), timeout=5)
    except grpc.RpcError:
        pass


def test_get_name_with_slash(gnmi_stub):
    """Get request of list item where name contains /.

    Equivalent to: "Get request of one list item where name contains /" [get]
    """
    name = "Gigabit5/0/0"
    _create_thing(gnmi_stub, name)

    path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test"),
        gnmi_pb2.PathElem(name="things", key={"name": name}),
    ])
    req = gnmi_pb2.GetRequest(
        path=[path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    assert len(resp.notification[0].update) == 1

    _delete_thing(gnmi_stub, name)


def test_get_name_with_bracket(gnmi_stub):
    """Get request of list item where name contains [.

    Equivalent to: "Get request of one list item where name contains [" [get]
    """
    name = "One[1]"
    _create_thing(gnmi_stub, name)

    path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test"),
        gnmi_pb2.PathElem(name="things", key={"name": name}),
    ])
    req = gnmi_pb2.GetRequest(
        path=[path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    assert len(resp.notification[0].update) == 1

    _delete_thing(gnmi_stub, name)


def test_get_name_with_single_quote(gnmi_stub):
    """Get request of list item where name contains '.

    Equivalent to: "Get request of one list item where name contains '" [get]
    """
    name = "it's"
    _create_thing(gnmi_stub, name)

    path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test"),
        gnmi_pb2.PathElem(name="things", key={"name": name}),
    ])
    req = gnmi_pb2.GetRequest(
        path=[path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    assert len(resp.notification[0].update) == 1

    _delete_thing(gnmi_stub, name)


def test_get_name_with_backslash(gnmi_stub):
    """Get request of list item where name contains \\.

    Equivalent to: "Get request of one list item where name contains \\\\" [get]
    """
    name = "back\\slash"
    _create_thing(gnmi_stub, name)

    path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test"),
        gnmi_pb2.PathElem(name="things", key={"name": name}),
    ])
    req = gnmi_pb2.GetRequest(
        path=[path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    assert len(resp.notification[0].update) == 1

    _delete_thing(gnmi_stub, name)


def test_get_composite_key(gnmi_stub):
    """Get request from list with composite key.

    Equivalent to: "Get request from list with composite key" [get-composite-key]
    """
    # Create a composite key entry via Set
    path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test3"),
        gnmi_pb2.PathElem(name="complex-list",
                          key={"type": "bar", "name": "foo"}),
        gnmi_pb2.PathElem(name="data"),
    ])
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"testdata"'),
        )],
    )
    gnmi_stub.Set(req, timeout=5)

    # Get it back
    get_path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test3"),
        gnmi_pb2.PathElem(name="complex-list",
                          key={"type": "bar", "name": "foo"}),
    ])
    get_req = gnmi_pb2.GetRequest(
        path=[get_path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(get_req, timeout=5)
    assert len(resp.notification) == 1
    assert len(resp.notification[0].update) == 1
    val = resp.notification[0].update[0].val.json_ietf_val.decode()
    assert "testdata" in val

    # Cleanup
    del_path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test3"),
    ])
    try:
        gnmi_stub.Set(gnmi_pb2.SetRequest(delete=[del_path]), timeout=5)
    except grpc.RpcError:
        pass


def test_get_name_with_double_quote(gnmi_stub):
    """Get request of list item where name contains double quote.

    Equivalent to: 'Get request of one list item where name contains "' [get]
    """
    name = 'say"hello'
    _create_thing(gnmi_stub, name)

    path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test"),
        gnmi_pb2.PathElem(name="things", key={"name": name}),
    ])
    req = gnmi_pb2.GetRequest(
        path=[path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    assert len(resp.notification[0].update) == 1

    _delete_thing(gnmi_stub, name)


def test_get_composite_key_with_slash(gnmi_stub):
    """Get request from list with composite key having slashes.

    Equivalent to: "Get request from list with composite key having slashes" [get]
    """
    path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test3"),
        gnmi_pb2.PathElem(name="complex-list",
                          key={"type": "a/b", "name": "c/d"}),
        gnmi_pb2.PathElem(name="data"),
    ])
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"slash_data"'),
        )],
    ), timeout=5)

    get_path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test3"),
        gnmi_pb2.PathElem(name="complex-list",
                          key={"type": "a/b", "name": "c/d"}),
    ])
    resp = gnmi_stub.Get(gnmi_pb2.GetRequest(
        path=[get_path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    ), timeout=5)
    assert len(resp.notification[0].update) == 1

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(delete=[gnmi_pb2.Path(
        origin="rfc7951",
        elem=[gnmi_pb2.PathElem(name="gnmi-server-test:test3")],
    )]), timeout=5)


def test_get_leaf_parent_with_slash(gnmi_stub):
    """Get request of one leaf where parent name contains /.

    Equivalent to: "Get request of one leaf where parent name contains /" [get]
    """
    name = "Gig5/0/1"
    _create_thing(gnmi_stub, name)

    path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test"),
        gnmi_pb2.PathElem(name="things", key={"name": name}),
        gnmi_pb2.PathElem(name="description"),
    ])
    req = gnmi_pb2.GetRequest(
        path=[path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    assert len(resp.notification[0].update) == 1

    _delete_thing(gnmi_stub, name)


def test_get_parallel(gnmi_stub):
    """Top-level multiple parallel Get requests.

    Equivalent to: "Top-level multiple parallel Get requests" [get]
    """
    import concurrent.futures

    path = xpath_to_path("/gnmi-server-test:test-state")

    def do_get(_):
        req = gnmi_pb2.GetRequest(
            path=[path],
            encoding=gnmi_pb2.JSON_IETF,
        )
        resp = gnmi_stub.Get(req, timeout=10)
        return len(resp.notification) == 1

    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as pool:
        results = list(pool.map(do_get, range(20)))

    assert all(results), f"Some parallel Gets failed: {results}"


def test_get_composite_key_with_double_quotes(gnmi_stub):
    """Get request from list with composite key having double-quotes.

    Equivalent to: "Get request from list with composite key having double-quotes" [get]
    """
    # Use single-quoted key value containing double quotes
    path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test3"),
        gnmi_pb2.PathElem(name="complex-list",
                          key={"type": 'say"hi', "name": "foo"}),
        gnmi_pb2.PathElem(name="data"),
    ])
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"dq_data"'),
        )],
    ), timeout=5)

    get_path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test3"),
        gnmi_pb2.PathElem(name="complex-list",
                          key={"type": 'say"hi', "name": "foo"}),
    ])
    resp = gnmi_stub.Get(gnmi_pb2.GetRequest(
        path=[get_path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    ), timeout=5)
    assert len(resp.notification[0].update) == 1

    gnmi_stub.Set(gnmi_pb2.SetRequest(delete=[gnmi_pb2.Path(
        origin="rfc7951",
        elem=[gnmi_pb2.PathElem(name="gnmi-server-test:test3")],
    )]), timeout=5)


def test_get_invalid_datatype(gnmi_stub):
    """Get request with invalid datatype.

    Equivalent to: "Get request with invalid datatype" [get-neg]
    """
    path = xpath_to_path("/gnmi-server-test:test-state")
    req = gnmi_pb2.GetRequest(
        path=[path],
        type=99,  # invalid enum
        encoding=gnmi_pb2.JSON_IETF,
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Get(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.UNIMPLEMENTED


def test_get_unsupported_encoding(gnmi_stub):
    """Get request with unsupported encoding type.

    Equivalent to: "Get request with unsupported encoding type" [get-neg]
    """
    path = xpath_to_path("/gnmi-server-test:test-state")
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.BYTES,
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Get(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.UNIMPLEMENTED


def test_get_use_models_specific_path(gnmi_stub):
    """Get a specific path with use_models matching its module.

    Equivalent to: "Get with use_models on specific path" [get-use-models]
    """
    path = xpath_to_path("/gnmi-server-test:test-state")
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
        use_models=[gnmi_pb2.ModelData(name="gnmi-server-test")],
    )
    resp = gnmi_stub.Get(req, timeout=10)
    assert len(resp.notification) == 1
    assert len(resp.notification[0].update) >= 1


def test_get_empty_origin(gnmi_stub):
    """Get request with empty origin - should work (default = rfc7951).

    Most gNMI clients (gnmic, gNMIc) don't set origin at all.
    The server should accept empty origin as the default.
    """
    path = gnmi_pb2.Path(
        origin="",
        elem=[gnmi_pb2.PathElem(name="gnmi-server-test:test-state")],
    )
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    assert resp.notification[0].timestamp > 0


def test_get_no_origin(gnmi_stub):
    """Get request with no origin set at all - should work."""
    path = gnmi_pb2.Path(
        elem=[gnmi_pb2.PathElem(name="gnmi-server-test:test-state")],
    )
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=5)
    assert len(resp.notification) == 1
    assert resp.notification[0].timestamp > 0


def test_get_wrong_origin(gnmi_stub):
    """Get request with unsupported origin - should fail."""
    path = gnmi_pb2.Path(
        origin="openconfig",
        elem=[gnmi_pb2.PathElem(name="gnmi-server-test:test-state")],
    )
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Get(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT
    assert "origin" in exc_info.value.details().lower()


def test_get_relative_path(gnmi_stub):
    """Get request with relative path.

    Equivalent to: "Get request with relative path" [get-neg]
    """
    path = gnmi_pb2.Path(
        origin="rfc7951",
        elem=[gnmi_pb2.PathElem(name="..")],
    )
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Get(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT
    assert "Relative" in exc_info.value.details()


def test_get_state_datatype(gnmi_stub):
    """Get request with DataType STATE.

    Equivalent to: "Get request with STATE datatype" [get-state]
    STATE returns operational (non-config) data.
    """
    path = xpath_to_path("/gnmi-server-test:test-state")
    req = gnmi_pb2.GetRequest(
        path=[path],
        type=gnmi_pb2.GetRequest.STATE,
        encoding=gnmi_pb2.JSON_IETF,
    )
    resp = gnmi_stub.Get(req, timeout=10)
    assert len(resp.notification) == 1
    assert len(resp.notification[0].update) >= 1


def test_get_use_models_single(gnmi_stub):
    """Get with use_models filtering to a single module.

    Equivalent to: "Get with use_models single module" [get-use-models]
    Only data from the specified module should be returned.
    """
    path = xpath_to_path("/*")
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
        use_models=[gnmi_pb2.ModelData(name="gnmi-server-test")],
    )
    resp = gnmi_stub.Get(req, timeout=10)
    assert len(resp.notification) == 1
    # Should have gnmi-server-test data but not other modules
    notif = resp.notification[0]
    for upd in notif.update:
        first_elem = upd.path.elem[0].name if upd.path.elem else ""
        assert "gnmi-server-test" in first_elem, (
            f"Expected only gnmi-server-test data, got {first_elem}"
        )


def test_get_use_models_filter_out(gnmi_stub):
    """Get with use_models for a nonexistent module.

    Equivalent to: "Get with use_models filter out" [get-use-models]
    No data should match, response should have empty updates.
    """
    path = xpath_to_path("/*")
    req = gnmi_pb2.GetRequest(
        path=[path],
        encoding=gnmi_pb2.JSON_IETF,
        use_models=[gnmi_pb2.ModelData(name="nonexistent-module")],
    )
    resp = gnmi_stub.Get(req, timeout=10)
    assert len(resp.notification) == 1
    assert len(resp.notification[0].update) == 0
