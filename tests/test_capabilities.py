"""
Capabilities RPC conformance tests.
"""

import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "proto"))

import grpc
import gnmi_pb2
import gnmi_pb2_grpc


def test_capability_request(gnmi_stub):
    """Capability request returns gNMI version, encodings, and models.

    """
    request = gnmi_pb2.CapabilityRequest()
    response = gnmi_stub.Capabilities(request, timeout=5)

    # gNMI version
    assert response.gNMI_version == "0.7.0"

    # JSON_IETF and JSON encodings supported
    assert len(response.supported_encodings) == 2
    assert gnmi_pb2.JSON_IETF in response.supported_encodings
    assert gnmi_pb2.JSON in response.supported_encodings

    # Test YANG modules should be present
    model_names = {m.name: m for m in response.supported_models}

    assert "gnmi-server-test" in model_names, (
        f"gnmi-server-test not found in models: {list(model_names.keys())}"
    )
    assert model_names["gnmi-server-test"].version == "2021-02-10"

    assert "gnmi-server-test-wine" in model_names
    assert model_names["gnmi-server-test-wine"].version == "2022-04-27"
