#!/bin/bash

# Write /opt/amnezia/version.json with current config metadata
# Variables replaced by client:
#   $CLIENT_VERSION — client app version
#   $CONFIG_SCHEMA_VERSION — integer schema version
#   $CONTAINER_TYPE — e.g. "xray", "awg2"
#   $CONTAINER_CONFIG_B64 — base64-encoded JSON (prevents shell injection)

VFILE="/opt/amnezia/version.json"

# Read existing version.json and base64-encode it ON the server
# This prevents any triple-quote / shell metachar injection from the file content
EXISTING_B64=$(cat "$VFILE" 2>/dev/null | base64 -w0 || echo "e30=")

python3 -c "\
import json, base64, datetime;\
existing = {};\
try:\
    existing = json.loads(base64.b64decode('$EXISTING_B64').decode('utf-8', errors='replace'));\
except: pass;\
container_info = {};\
try:\
    container_info = json.loads(base64.b64decode('$CONTAINER_CONFIG_B64').decode('utf-8', errors='replace'));\
except: pass;\
now = datetime.datetime.utcnow().strftime('%Y-%m-%dT%H:%M:%SZ');\
existing['clientVersion'] = '$CLIENT_VERSION';\
existing['configSchemaVersion'] = $CONFIG_SCHEMA_VERSION;\
existing['lastUpdated'] = now;\
container_info['configuredAt'] = now;\
existing['$CONTAINER_TYPE'] = container_info;\
print(json.dumps(existing, indent=2))\
" > "$VFILE.tmp" && mv "$VFILE.tmp" "$VFILE"

echo "Version file updated: $VFILE"
cat "$VFILE"
