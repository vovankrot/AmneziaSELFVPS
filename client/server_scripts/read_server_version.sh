# Read /opt/amnezia/version.json from server (one-liner for runScript line-by-line SSH execution)
cat /opt/amnezia/version.json 2>/dev/null || echo "{}"
