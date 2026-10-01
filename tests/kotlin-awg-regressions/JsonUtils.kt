package org.amnezia.vpn.util
import org.json.JSONObject
fun JSONObject.optStringOrNull(key: String) = values[key]
