#!/usr/bin/env bash
set -euo pipefail

project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cert_dir="$project_root/main/certs"
mkdir -p "$cert_dir"

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 365 \
  -subj "/CN=mouth.local" \
  -addext "basicConstraints=critical,CA:FALSE" \
  -addext "subjectAltName=DNS:mouth.local,IP:192.168.4.1" \
  -addext "keyUsage=critical,digitalSignature,keyEncipherment" \
  -addext "extendedKeyUsage=serverAuth" \
  -keyout "$cert_dir/prvtkey.pem" \
  -out "$cert_dir/servercert.pem"

echo "Generated a development-only self-signed certificate in $cert_dir"
