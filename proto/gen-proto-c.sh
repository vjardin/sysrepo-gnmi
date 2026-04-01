#!/bin/sh
# Generate protobuf-c stubs for gNMI protos + well-known types
# Args: $1=proto_dir $2=proto_inc $3=outdir
set -e
PROTO_DIR="$1"
PROTO_INC="$2"
OUTDIR="$3"

mkdir -p "$OUTDIR"

# Generate well-known types into outdir (preserving google/protobuf/ subdir)
protoc --proto_path="$PROTO_INC" --c_out="$OUTDIR" \
    "$PROTO_INC/google/protobuf/any.proto" \
    "$PROTO_INC/google/protobuf/descriptor.proto"

# Generate gNMI protos
protoc --proto_path="$PROTO_DIR" --proto_path="$PROTO_INC" --c_out="$OUTDIR" \
    "$PROTO_DIR/gnmi.proto" \
    "$PROTO_DIR/gnmi_ext.proto"
