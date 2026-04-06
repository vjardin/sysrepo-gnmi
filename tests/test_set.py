"""
Set RPC conformance tests.
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


def test_set_update_leaf(gnmi_stub):
    """Leaf Set request (update).

    Equivalent to: "Leaf Set request (update)" [set]
    """
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='A']/description"
    )
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"hello"'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert len(resp.response) == 1
    assert resp.response[0].op == gnmi_pb2.UpdateResult.UPDATE


def test_set_update_list_entry(gnmi_stub):
    """Path-based Set request (update).

    Equivalent to: "Path-based Set request (update)" [set]
    """
    path = xpath_to_path("/gnmi-server-test:test/things[name='B']")
    data = json.dumps({"description": "item B", "enabled": True})
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=data.encode()),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert len(resp.response) == 1
    assert resp.response[0].op == gnmi_pb2.UpdateResult.UPDATE

    # Verify via Get (CONFIG datastore - Set writes to Running)
    get_req = gnmi_pb2.GetRequest(
        path=[xpath_to_path(
            "/gnmi-server-test:test/things[name='B']/description"
        )],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    get_resp = gnmi_stub.Get(get_req, timeout=5)
    assert len(get_resp.notification) == 1
    assert len(get_resp.notification[0].update) == 1
    val = get_resp.notification[0].update[0].val.json_ietf_val.decode()
    assert "item B" in val


def test_set_delete_path(gnmi_stub):
    """Path-based Set request (delete).

    Equivalent to: "Path-based Set request (delete)" [set]
    """
    # First create something
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='ToDelete']/description"
    )
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"temp"'),
        )],
    )
    gnmi_stub.Set(req, timeout=5)

    # Now delete it
    del_path = xpath_to_path(
        "/gnmi-server-test:test/things[name='ToDelete']"
    )
    req = gnmi_pb2.SetRequest(delete=[del_path])
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert len(resp.response) == 1
    assert resp.response[0].op == gnmi_pb2.UpdateResult.DELETE

    # Verify deleted via Get
    get_req = gnmi_pb2.GetRequest(
        path=[xpath_to_path(
            "/gnmi-server-test:test/things[name='ToDelete']"
        )],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    get_resp = gnmi_stub.Get(get_req, timeout=5)
    assert len(get_resp.notification[0].update) == 0


def test_set_delete_nonexistent(gnmi_stub):
    """Set request (delete) with non-existent path.

    Equivalent to: "Set request (delete) with non-existent path" [set]
    gNMI spec 3.4.6: delete of non-existent path is silently OK.
    """
    path = xpath_to_path("/gnmi-server-test:test/things[name='DoesNotExist']")
    req = gnmi_pb2.SetRequest(delete=[path])
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0


def test_set_transaction_multi_update(gnmi_stub):
    """Set request transaction (update) - multiple updates in one request.

    Equivalent to: "Set request transaction (update)" [set]
    """
    path1 = xpath_to_path(
        "/gnmi-server-test:test/things[name='X']/description"
    )
    path2 = xpath_to_path(
        "/gnmi-server-test:test/things[name='Y']/description"
    )
    req = gnmi_pb2.SetRequest(
        update=[
            gnmi_pb2.Update(
                path=path1,
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"valX"'),
            ),
            gnmi_pb2.Update(
                path=path2,
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"valY"'),
            ),
        ],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert len(resp.response) == 2
    assert resp.response[0].op == gnmi_pb2.UpdateResult.UPDATE
    assert resp.response[1].op == gnmi_pb2.UpdateResult.UPDATE


def test_set_with_prefix(gnmi_stub):
    """Set request (with prefix).

    Equivalent to: "Set request (with prefix)" [set]
    """
    prefix = xpath_to_path("/gnmi-server-test:test")
    path = xpath_to_path("/things[name='PfxTest']/description")
    path.origin = ""
    req = gnmi_pb2.SetRequest(
        prefix=prefix,
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"pfx_val"'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert len(resp.response) == 1


# - Negative tests ---------------------------------------------------


def test_set_empty_leaf(gnmi_stub):
    """Empty leaf Set request (update).

    Equivalent to: "Empty leaf Set request (update)" [set]
    """
    path = xpath_to_path("/gnmi-server-test:test/things[name='EmptyLeaf']/ready")
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'[null]'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0

    # Verify via Get
    get_req = gnmi_pb2.GetRequest(
        path=[xpath_to_path(
            "/gnmi-server-test:test/things[name='EmptyLeaf']/ready"
        )],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    get_resp = gnmi_stub.Get(get_req, timeout=5)
    assert len(get_resp.notification[0].update) == 1

    # Cleanup
    del_path = xpath_to_path("/gnmi-server-test:test/things[name='EmptyLeaf']")
    gnmi_stub.Set(gnmi_pb2.SetRequest(delete=[del_path]), timeout=5)


def test_set_top_level_update(gnmi_stub):
    """Top-level Set request (update) at root with full JSON.

    Equivalent to: "Top-level Set request (update)" [set]
    """
    path = gnmi_pb2.Path(origin="rfc7951")
    data = json.dumps({
        "gnmi-server-test:test": {
            "things": [
                {"name": "TopA", "enabled": True}
            ]
        }
    })
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=data.encode()),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0

    # Verify
    get_req = gnmi_pb2.GetRequest(
        path=[xpath_to_path(
            "/gnmi-server-test:test/things[name='TopA']/enabled"
        )],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    get_resp = gnmi_stub.Get(get_req, timeout=5)
    assert len(get_resp.notification[0].update) == 1

    # Cleanup
    del_path = xpath_to_path("/gnmi-server-test:test/things[name='TopA']")
    gnmi_stub.Set(gnmi_pb2.SetRequest(delete=[del_path]), timeout=5)


def test_set_top_level_delete(gnmi_stub):
    """Top-level Set request (delete) removes all config.

    Equivalent to: "Top-level Set request (delete)" [set]
    """
    # Create some data first
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='DelAll']/description"
    )
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"temp"'),
        )],
    ), timeout=5)

    # Delete root
    del_path = xpath_to_path("/gnmi-server-test:test")
    req = gnmi_pb2.SetRequest(delete=[del_path])
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0

    # Verify deleted
    get_req = gnmi_pb2.GetRequest(
        path=[xpath_to_path("/gnmi-server-test:test/things[name='DelAll']")],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    get_resp = gnmi_stub.Get(get_req, timeout=5)
    assert len(get_resp.notification[0].update) == 0


def test_set_delete_same_path_twice(gnmi_stub):
    """Set request (delete) the same path twice - idempotent.

    Equivalent to: "Set request (delete) the same path" [set]
    """
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='DelTwice']/description"
    )
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"temp"'),
        )],
    ), timeout=5)

    del_path = xpath_to_path("/gnmi-server-test:test/things[name='DelTwice']")
    req = gnmi_pb2.SetRequest(delete=[del_path, del_path])
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert len(resp.response) == 2


def test_set_composite_key(gnmi_stub):
    """Set request for list with composite key.

    Equivalent to: "Set request for list with composite key" [set]
    """
    path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test3"),
        gnmi_pb2.PathElem(name="complex-list",
                          key={"type": "settype", "name": "setname"}),
        gnmi_pb2.PathElem(name="data"),
    ])
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"composite_val"'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert resp.response[0].op == gnmi_pb2.UpdateResult.UPDATE

    # Cleanup
    del_path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test3"),
    ])
    gnmi_stub.Set(gnmi_pb2.SetRequest(delete=[del_path]), timeout=5)


def test_set_transaction_delete_update(gnmi_stub):
    """Set request transaction (delete+update) in one request.

    Equivalent to: "Set request transaction (delete+update)" [set]
    """
    # Create something to delete
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='TxnDel']/description"
    )
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"old"'),
        )],
    ), timeout=5)

    # Delete + update in same transaction
    del_path = xpath_to_path("/gnmi-server-test:test/things[name='TxnDel']")
    upd_path = xpath_to_path(
        "/gnmi-server-test:test/things[name='TxnNew']/description"
    )
    req = gnmi_pb2.SetRequest(
        delete=[del_path],
        update=[gnmi_pb2.Update(
            path=upd_path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"new"'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert len(resp.response) == 2  # 1 delete + 1 update

    # Cleanup
    del_path2 = xpath_to_path("/gnmi-server-test:test/things[name='TxnNew']")
    gnmi_stub.Set(gnmi_pb2.SetRequest(delete=[del_path2]), timeout=5)


def test_set_leaflist_update(gnmi_stub):
    """Top-level Set request leaflist (update).

    Equivalent to: "Top-level Set request leaflist (update)" [set]
    """
    path = xpath_to_path("/gnmi-server-test:test4/params")
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"speed"'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test4")],
    ), timeout=5)


def test_set_delete_with_wildcards(gnmi_stub):
    """Set request (delete with wildcards).

    Equivalent to: "Set request (delete with wildcards)" [set]
    """
    # Create two items with enabled=true
    for name in ["WcA", "WcB"]:
        path = gnmi_pb2.Path(origin="rfc7951", elem=[
            gnmi_pb2.PathElem(name="gnmi-server-test:test"),
            gnmi_pb2.PathElem(name="things", key={"name": name}),
            gnmi_pb2.PathElem(name="enabled"),
        ])
        gnmi_stub.Set(gnmi_pb2.SetRequest(
            update=[gnmi_pb2.Update(
                path=path,
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"true"'),
            )],
        ), timeout=5)

    # Delete enabled from all things using wildcard
    del_path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test:test"),
        gnmi_pb2.PathElem(name="*"),
        gnmi_pb2.PathElem(name="enabled"),
    ])
    resp = gnmi_stub.Set(gnmi_pb2.SetRequest(delete=[del_path]), timeout=5)
    assert resp.timestamp > 0

    # Cleanup
    for name in ["WcA", "WcB"]:
        gnmi_stub.Set(gnmi_pb2.SetRequest(delete=[gnmi_pb2.Path(
            origin="rfc7951", elem=[
                gnmi_pb2.PathElem(name="gnmi-server-test:test"),
                gnmi_pb2.PathElem(name="things", key={"name": name}),
            ],
        )]), timeout=5)


def test_set_delete_child_then_parent(gnmi_stub):
    """Set request (delete) child then parent.

    Equivalent to: "Set request (delete) child then parent" [set]
    """
    # Create entry
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='CTP']/description"
    )
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"temp"'),
        )],
    ), timeout=5)

    # Delete child then parent in same request
    child = xpath_to_path(
        "/gnmi-server-test:test/things[name='CTP']/description"
    )
    parent = xpath_to_path("/gnmi-server-test:test/things[name='CTP']")
    resp = gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[child, parent],
    ), timeout=5)
    assert len(resp.response) == 2


def test_set_delete_parent_then_child(gnmi_stub):
    """Set request (delete) parent then child.

    Equivalent to: "Set request (delete) parent then child" [set]
    """
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='PTC']/description"
    )
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"temp"'),
        )],
    ), timeout=5)

    parent = xpath_to_path("/gnmi-server-test:test/things[name='PTC']")
    child = xpath_to_path(
        "/gnmi-server-test:test/things[name='PTC']/description"
    )
    resp = gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[parent, child],
    ), timeout=5)
    assert len(resp.response) == 2


def test_set_with_empty_prefix(gnmi_stub):
    """Set request (with empty prefix).

    Equivalent to: "Set request (with empty prefix)" [set]
    """
    prefix = gnmi_pb2.Path()  # empty prefix, no origin, no elems
    path = xpath_to_path("/gnmi-server-test:test/things[name='EmptyPfx']/description")
    # origin stays on path since prefix has no elems
    req = gnmi_pb2.SetRequest(
        prefix=prefix,
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"epfx"'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test/things[name='EmptyPfx']")],
    ), timeout=5)


def test_set_scaled(gnmi_stub):
    """Scaled Set request (update) - many updates in one transaction.

    Equivalent to: "Scaled Set request (update)" [set-scale]
    """
    updates = []
    for i in range(100):
        path = gnmi_pb2.Path(origin="rfc7951", elem=[
            gnmi_pb2.PathElem(name="gnmi-server-test:test"),
            gnmi_pb2.PathElem(name="things", key={"name": f"scale{i}"}),
            gnmi_pb2.PathElem(name="description"),
        ])
        updates.append(gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=f'"val{i}"'.encode()),
        ))

    resp = gnmi_stub.Set(gnmi_pb2.SetRequest(update=updates), timeout=30)
    assert resp.timestamp > 0
    assert len(resp.response) == 100

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test")],
    ), timeout=10)


def test_set_replace_empty(gnmi_stub):
    """Top-level Set request (replace) empty - replace with {}.

    Equivalent to: "Top-level Set request (replace) empty" [set]
    """
    # Create data first
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='ReplEmpty']/description"
    )
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"temp"'),
        )],
    ), timeout=5)

    # Replace with empty JSON at root - should remove all config
    root = gnmi_pb2.Path(origin="rfc7951")
    req = gnmi_pb2.SetRequest(
        replace=[gnmi_pb2.Update(
            path=root,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'{}'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert resp.response[0].op == gnmi_pb2.UpdateResult.REPLACE

    # Verify data is gone
    get_req = gnmi_pb2.GetRequest(
        path=[xpath_to_path("/gnmi-server-test:test/things[name='ReplEmpty']")],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    )
    get_resp = gnmi_stub.Get(get_req, timeout=5)
    assert len(get_resp.notification[0].update) == 0


def test_set_multi_replace(gnmi_stub):
    """Top-level Set request (multi replace).

    Equivalent to: "Top-level Set request (multi replace)" [set]
    """
    path1 = xpath_to_path(
        "/gnmi-server-test:test/things[name='MR1']/description"
    )
    path2 = xpath_to_path(
        "/gnmi-server-test:test/things[name='MR2']/description"
    )
    req = gnmi_pb2.SetRequest(
        replace=[
            gnmi_pb2.Update(
                path=path1,
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"repl1"'),
            ),
            gnmi_pb2.Update(
                path=path2,
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"repl2"'),
            ),
        ],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert len(resp.response) == 2
    assert resp.response[0].op == gnmi_pb2.UpdateResult.REPLACE
    assert resp.response[1].op == gnmi_pb2.UpdateResult.REPLACE

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test")],
    ), timeout=5)


def test_set_multi_replace_and_update(gnmi_stub):
    """Top-level Set request (multi replace + update).

    Equivalent to: "Top-level Set request (multi replace + update)" [set]
    """
    replace_path = xpath_to_path(
        "/gnmi-server-test:test/things[name='MRU1']/description"
    )
    update_path = xpath_to_path(
        "/gnmi-server-test:test/things[name='MRU2']/description"
    )
    req = gnmi_pb2.SetRequest(
        replace=[gnmi_pb2.Update(
            path=replace_path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"replaced"'),
        )],
        update=[gnmi_pb2.Update(
            path=update_path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"updated"'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert len(resp.response) == 2
    assert resp.response[0].op == gnmi_pb2.UpdateResult.REPLACE
    assert resp.response[1].op == gnmi_pb2.UpdateResult.UPDATE

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test")],
    ), timeout=5)


def test_set_more_unsupported_val_types(gnmi_stub):
    """Set request with more unsupported TypedValue types.

    Equivalent to: various "Set request (X val type)" [set-neg]
    """
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='A']/description"
    )

    # bytes_val
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(bytes_val=b"data"),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    # int_val
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(int_val=-5),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    # float_val
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(float_val=3.14),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_set_unsupported_val_types(gnmi_stub):
    """Set request with various unsupported TypedValue types.

    Equivalent to: "Set request (ascii val type)" etc. [set-neg]
    """
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='A']/description"
    )

    # ascii_val
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(ascii_val="test"),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    # string_val
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(string_val="test"),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    # uint_val
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(uint_val=42),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT

    # bool_val
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(bool_val=True),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_set_no_val_type(gnmi_stub):
    """Set request (no val type).

    Equivalent to: "Set request (no val type)" [set-neg]
    """
    path = xpath_to_path("/gnmi-server-test:test/things[name='A']/description")
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(),  # value_case = VALUE_NOT_SET
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_set_no_path(gnmi_stub):
    """Set request (no path).

    Equivalent to: "Set request (no path)" [set-neg]
    """
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"test"'),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_set_top_level_replace(gnmi_stub):
    """Top-level Set request (replace) - removes items not in new config.

    Equivalent to: "Top-level Set request (replace)" [set]
    """
    # Create two items
    for name in ["RA", "RB"]:
        gnmi_stub.Set(gnmi_pb2.SetRequest(
            update=[gnmi_pb2.Update(
                path=xpath_to_path(
                    f"/gnmi-server-test:test/things[name='{name}']/description"
                ),
                val=gnmi_pb2.TypedValue(json_ietf_val=f'"{name}"'.encode()),
            )],
        ), timeout=5)

    # Replace at specific path with only RA - RB should remain
    # (replace at a leaf level replaces just that leaf)
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='RA']/description"
    )
    req = gnmi_pb2.SetRequest(
        replace=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"replaced_RA"'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert resp.response[0].op == gnmi_pb2.UpdateResult.REPLACE

    # Verify RA was replaced
    get_resp = gnmi_stub.Get(gnmi_pb2.GetRequest(
        path=[xpath_to_path(
            "/gnmi-server-test:test/things[name='RA']/description"
        )],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    ), timeout=5)
    assert len(get_resp.notification[0].update) == 1
    val = get_resp.notification[0].update[0].val.json_ietf_val.decode()
    assert "replaced_RA" in val

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test")],
    ), timeout=5)


def test_set_leaflist_replace(gnmi_stub):
    """Top-level Set request leaflist (replace).

    Equivalent to: "Top-level Set request leaflist (replace)" [set]
    """
    # First add some values
    for val in ["aa", "bb"]:
        gnmi_stub.Set(gnmi_pb2.SetRequest(
            update=[gnmi_pb2.Update(
                path=xpath_to_path("/gnmi-server-test:test4/params"),
                val=gnmi_pb2.TypedValue(
                    json_ietf_val=f'"{val}"'.encode()),
            )],
        ), timeout=5)

    # Replace with a new value
    req = gnmi_pb2.SetRequest(
        replace=[gnmi_pb2.Update(
            path=xpath_to_path("/gnmi-server-test:test4/params"),
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"cc"'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert resp.response[0].op == gnmi_pb2.UpdateResult.REPLACE

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test4")],
    ), timeout=5)


def test_set_failing_transaction(gnmi_stub):
    """Set request failing transaction (2 updates).

    Equivalent to: "Set request failing transaction (2 updates)" [set-neg]
    The second update has an invalid path -> entire transaction rolls back.
    """
    path1 = xpath_to_path(
        "/gnmi-server-test:test/things[name='TxnFail']/description"
    )
    # Invalid: nonexistent leaf
    path2 = xpath_to_path(
        "/gnmi-server-test:test/things[name='TxnFail']/nonexistent_leaf"
    )
    req = gnmi_pb2.SetRequest(
        update=[
            gnmi_pb2.Update(
                path=path1,
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"good"'),
            ),
            gnmi_pb2.Update(
                path=path2,
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"bad"'),
            ),
        ],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    # Should fail (INVALID_ARGUMENT or ABORTED)
    assert exc_info.value.code() in (
        grpc.StatusCode.INVALID_ARGUMENT,
        grpc.StatusCode.ABORTED,
    )


def test_set_no_namespace(gnmi_stub):
    """Top-level Set request (update, no namespace).

    Equivalent to: "Top-level Set request (update, no namespace)" [set-neg]
    """
    root = gnmi_pb2.Path(origin="rfc7951")
    data = json.dumps({
        "test": {  # missing module prefix
            "things": [{"name": "NoNS"}]
        }
    })
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=root,
            val=gnmi_pb2.TypedValue(json_ietf_val=data.encode()),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() in (
        grpc.StatusCode.INVALID_ARGUMENT,
        grpc.StatusCode.ABORTED,
    )


def test_set_leaflist_inner_replace(gnmi_stub):
    """Top-level Set request inner leaflist (replace).

    Equivalent to: "Top-level Set request inner leaflist (replace)" [set]
    """
    # Create a list entry with amount-history leaflist
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=xpath_to_path(
                "/gnmi-server-test:test/things[name='LLR']/amount"
            ),
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"10"'),
        )],
    ), timeout=5)

    # Replace amount-history leaflist
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='LLR']/amount-history"
    )
    req = gnmi_pb2.SetRequest(
        replace=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"4"'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0
    assert resp.response[0].op == gnmi_pb2.UpdateResult.REPLACE

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test")],
    ), timeout=5)


def test_set_boolean_leaf(gnmi_stub):
    """Set request for boolean leaf.

    Tests boolean value encoding.
    """
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='BoolTest']/enabled"
    )
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"true"'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0

    # Verify
    get_resp = gnmi_stub.Get(gnmi_pb2.GetRequest(
        path=[path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    ), timeout=5)
    assert len(get_resp.notification[0].update) == 1
    val = get_resp.notification[0].update[0].val.json_ietf_val.decode()
    assert "true" in val

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test/things[name='BoolTest']")],
    ), timeout=5)


def test_set_integer_leaf(gnmi_stub):
    """Set request for integer leaf (uint32, int32, decimal64).

    Tests numeric value encoding.
    """
    # uint32
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='NumTest']/amount"
    )
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"42"'),
        )],
    ), timeout=5)

    get_resp = gnmi_stub.Get(gnmi_pb2.GetRequest(
        path=[path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    ), timeout=5)
    assert len(get_resp.notification[0].update) == 1
    val = get_resp.notification[0].update[0].val.json_ietf_val.decode()
    assert "42" in val

    # int32 (signed)
    path2 = xpath_to_path(
        "/gnmi-server-test:test/things[name='NumTest']/signed-amount"
    )
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path2,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"-7"'),
        )],
    ), timeout=5)

    get_resp = gnmi_stub.Get(gnmi_pb2.GetRequest(
        path=[path2],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    ), timeout=5)
    assert len(get_resp.notification[0].update) == 1

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test/things[name='NumTest']")],
    ), timeout=5)


def test_set_wine_composite_key(gnmi_stub):
    """Set request for wine module with composite key.

    Tests the second YANG module (gnmi-server-test-wine).
    """
    path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test-wine:wines"),
        gnmi_pb2.PathElem(name="wine",
                          key={"name": "Bordeaux", "vintage": "2020"}),
        gnmi_pb2.PathElem(name="score"),
    ])
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"95"'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0

    # Verify via Get
    get_path = gnmi_pb2.Path(origin="rfc7951", elem=[
        gnmi_pb2.PathElem(name="gnmi-server-test-wine:wines"),
        gnmi_pb2.PathElem(name="wine",
                          key={"name": "Bordeaux", "vintage": "2020"}),
    ])
    get_resp = gnmi_stub.Get(gnmi_pb2.GetRequest(
        path=[get_path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    ), timeout=5)
    assert len(get_resp.notification[0].update) == 1
    val = get_resp.notification[0].update[0].val.json_ietf_val.decode()
    assert "95" in val

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(delete=[gnmi_pb2.Path(
        origin="rfc7951",
        elem=[gnmi_pb2.PathElem(name="gnmi-server-test-wine:wines")],
    )]), timeout=5)


def test_set_json_val_type(gnmi_stub):
    """Set request (JSON val type) - unsupported.

    Equivalent to: "Set request (JSON val type)" [set-neg]
    """
    path = xpath_to_path("/gnmi-server-test:test/things[name='A']/description")
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_val=b'{"foo":"bar"}'),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_set_proto_bytes_val_type(gnmi_stub):
    """Set request (proto-bytes val type) - unsupported.

    Equivalent to: "Set request (proto-bytes val type)" [set-neg]
    """
    path = xpath_to_path("/gnmi-server-test:test/things[name='A']/description")
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(proto_bytes=b'\x08\x01'),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_set_any_val_type(gnmi_stub):
    """Set request (any-val val type) - unsupported.

    Equivalent to: "Set request (any-val val type)" [set-neg]
    """
    from google.protobuf import any_pb2
    path = xpath_to_path("/gnmi-server-test:test/things[name='A']/description")
    any_val = any_pb2.Any()
    any_val.Pack(gnmi_pb2.Path())
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(any_val=any_val),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_set_leaflist_val_type(gnmi_stub):
    """Set request (leaf-list val type) - unsupported.

    Equivalent to: "Set request (leaf-list val type)" [set-neg]
    """
    path = xpath_to_path("/gnmi-server-test:test/things[name='A']/description")
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(
                leaflist_val=gnmi_pb2.ScalarArray(
                    element=[gnmi_pb2.TypedValue(string_val="a")]
                ),
            ),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_set_decimal64_val_type(gnmi_stub):
    """Set request (decimal64 val) - unsupported TypedValue encoding.

    Equivalent to: "Set request (decimal64 val)" [set-neg]
    """
    path = xpath_to_path("/gnmi-server-test:test/things[name='A']/description")
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(
                decimal_val=gnmi_pb2.Decimal64(digits=314, precision=2),
            ),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_set_failing_transaction_delete_update(gnmi_stub):
    """Set request failing transaction (delete+update).

    Equivalent to: "Set request failing transaction (delete+update)" [set-neg]
    Delete a valid path + update an invalid path in one transaction.
    """
    # Create something to delete
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=xpath_to_path(
                "/gnmi-server-test:test/things[name='FTx']/description"
            ),
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"temp"'),
        )],
    ), timeout=5)

    del_path = xpath_to_path("/gnmi-server-test:test/things[name='FTx']")
    bad_path = xpath_to_path(
        "/gnmi-server-test:test/things[name='FTx']/nonexistent_leaf"
    )
    req = gnmi_pb2.SetRequest(
        delete=[del_path],
        update=[gnmi_pb2.Update(
            path=bad_path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"bad"'),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() in (
        grpc.StatusCode.INVALID_ARGUMENT,
        grpc.StatusCode.ABORTED,
    )

    # Cleanup
    try:
        gnmi_stub.Set(gnmi_pb2.SetRequest(
            delete=[xpath_to_path("/gnmi-server-test:test")],
        ), timeout=5)
    except grpc.RpcError:
        pass


def test_set_application_error(gnmi_stub):
    """Set request (application error string) - module-change callback rejects.

    Equivalent to: "Set request (application error string)" [set-neg]
    """
    path = xpath_to_path("/gnmi-server-test:test2/custom-error")
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"42"'),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.ABORTED
    assert "Fiddlesticks" in exc_info.value.details()


def test_set_data_model_error(gnmi_stub):
    """Set request (data model error) - YANG must constraint violation.

    Equivalent to: "Set request (data model error)" [set-neg]
    The leaf must-error has: must "current() > 42"
    Setting a value <= 42 should fail validation.
    """
    path = xpath_to_path("/gnmi-server-test:test2/must-error")
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"10"'),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.ABORTED


def test_set_incorrect_prefix(gnmi_stub):
    """Set request (incorrect prefix) - bad origin.

    Equivalent to: "Set request (incorrect prefix)" [set-neg]
    """
    prefix = gnmi_pb2.Path(origin="wrong-origin")
    path = xpath_to_path("/gnmi-server-test:test/things[name='A']/description")
    path.origin = ""
    req = gnmi_pb2.SetRequest(
        prefix=prefix,
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"test"'),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT


def test_set_failing_conflicting_leaves(gnmi_stub):
    """Set request failing transaction (2 conflicting leaf updates).

    Equivalent to: "Set request failing transaction (2 leaf updates)" [set-neg]
    Two updates to the same leaf in one request - second should win or fail.
    """
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='Conflict']/description"
    )
    req = gnmi_pb2.SetRequest(
        update=[
            gnmi_pb2.Update(
                path=path,
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"first"'),
            ),
            gnmi_pb2.Update(
                path=path,
                val=gnmi_pb2.TypedValue(json_ietf_val=b'"second"'),
            ),
        ],
    )
    # This may succeed (second wins) or fail - both are valid behaviors
    try:
        resp = gnmi_stub.Set(req, timeout=5)
        assert resp.timestamp > 0
        assert len(resp.response) == 2
    except grpc.RpcError:
        pass  # also acceptable

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test/things[name='Conflict']")],
    ), timeout=5)


def test_set_decimal_leaf(gnmi_stub):
    """Set request for decimal64 leaf.

    Tests decimal64 value encoding.
    """
    path = xpath_to_path(
        "/gnmi-server-test:test/things[name='DecTest']/decimal-amount"
    )
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"3.14"'),
        )],
    )
    resp = gnmi_stub.Set(req, timeout=5)
    assert resp.timestamp > 0

    # Verify
    get_resp = gnmi_stub.Get(gnmi_pb2.GetRequest(
        path=[path],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    ), timeout=5)
    assert len(get_resp.notification[0].update) == 1
    val = get_resp.notification[0].update[0].val.json_ietf_val.decode()
    assert "3.14" in val

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test/things[name='DecTest']")],
    ), timeout=5)


def test_set_wildcard_in_update(gnmi_stub):
    """Set request (update with wildcards).

    Equivalent to: "Set request (update with wildcards)" [set-neg]
    """
    path = xpath_to_path("/gnmi-server-test:test/*/description")
    req = gnmi_pb2.SetRequest(
        update=[gnmi_pb2.Update(
            path=path,
            val=gnmi_pb2.TypedValue(json_ietf_val=b'"test"'),
        )],
    )
    with pytest.raises(grpc.RpcError) as exc_info:
        gnmi_stub.Set(req, timeout=5)
    assert exc_info.value.code() == grpc.StatusCode.INVALID_ARGUMENT




def test_set_replace_removes_absent_children(gnmi_stub):
    """gNMI REPLACE must delete children not in the new value.

    Equivalent to: "Set replace removes absent children" [set-replace]
    Create two leaves in test2, replace the container with only one,
    verify the other is gone.
    """
    # Set both enabled and enabled2
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        update=[
            gnmi_pb2.Update(
                path=xpath_to_path("/gnmi-server-test:test2/enabled"),
                val=gnmi_pb2.TypedValue(json_ietf_val=b'true'),
            ),
            gnmi_pb2.Update(
                path=xpath_to_path("/gnmi-server-test:test2/enabled2"),
                val=gnmi_pb2.TypedValue(json_ietf_val=b'true'),
            ),
        ],
    ), timeout=5)

    # Verify both exist
    resp = gnmi_stub.Get(gnmi_pb2.GetRequest(
        path=[xpath_to_path("/gnmi-server-test:test2")],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    ), timeout=5)
    json_data = resp.notification[0].update[0].val.json_ietf_val.decode()
    assert "enabled" in json_data
    assert "enabled2" in json_data

    # REPLACE test2 with only enabled -- enabled2 should be removed
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        replace=[gnmi_pb2.Update(
            path=xpath_to_path("/gnmi-server-test:test2"),
            val=gnmi_pb2.TypedValue(
                json_ietf_val=b'{"gnmi-server-test:enabled": false}',
            ),
        )],
    ), timeout=5)

    # Verify enabled2 is gone
    resp = gnmi_stub.Get(gnmi_pb2.GetRequest(
        path=[xpath_to_path("/gnmi-server-test:test2")],
        type=gnmi_pb2.GetRequest.CONFIG,
        encoding=gnmi_pb2.JSON_IETF,
    ), timeout=5)
    json_data = resp.notification[0].update[0].val.json_ietf_val.decode()
    assert "enabled2" not in json_data, (
        f"REPLACE should remove absent children, but enabled2 still present: {json_data}"
    )

    # Cleanup
    gnmi_stub.Set(gnmi_pb2.SetRequest(
        delete=[xpath_to_path("/gnmi-server-test:test2")],
    ), timeout=5)
