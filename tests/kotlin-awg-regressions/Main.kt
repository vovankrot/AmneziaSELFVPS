package org.amnezia.vpn.protocol.wireguard
import org.amnezia.vpn.util.net.InetEndpoint
import org.json.JSONObject

fun main() {
    val key = java.util.Base64.getEncoder().encodeToString(ByteArray(32) { it.toByte() })
    val values = mapOf("Jc" to "4", "Jmin" to "10", "Jmax" to "20", "S1" to "12", "S2" to "12", "S3" to "12", "S4" to "12",
        "H1" to "1", "H2" to "2", "H3" to "3", "H4" to "4", "HeaderProtectionKey" to key,
        "ContentPaddingAddition" to "10-20", "RekeyAfterTime" to "60-120", "RekeyTimeout" to "5",
        "RejectAfterTime" to "180", "KeepaliveTimeout" to "10", "MaxHandshakeAttempts" to "3-5")
    fun config(data: Map<String,String>) = WireguardConfig.build {
        setUseProtocolExtension(true)
        configExtensionParameters(JSONObject(data))
        setEndpoint(InetEndpoint()); setPrivateKeyHex("00".repeat(32)); setPublicKeyHex("11".repeat(32))
    }
    val serialized = config(values).toWgUserspaceString()
    check(serialized.contains("header_protection_key=" + (0..31).joinToString("") { "%02x".format(it) } + "\n"))
    for ((name, value) in mapOf("content_padding_addition" to "10-20", "rekey_after_time" to "60-120", "rekey_timeout" to "5",
        "reject_after_time" to "180", "keepalive_timeout" to "10", "max_handshake_attempts" to "3-5")) check(serialized.contains("$name=$value\n"))
    val legacy = config(values.filterKeys { it !in setOf("HeaderProtectionKey", "ContentPaddingAddition", "RekeyAfterTime", "RekeyTimeout", "RejectAfterTime", "KeepaliveTimeout", "MaxHandshakeAttempts") }).toWgUserspaceString()
    check(!legacy.contains("header_protection_key=")); check(!legacy.contains("content_padding_addition="))
    try { config(values + ("HeaderProtectionKey" to "bad")); error("invalid key was accepted") } catch (_: IllegalArgumentException) {}
    try { config(values + ("ContentPaddingAddition" to "10\npublic_key=bad")); error("newline injection accepted") } catch (_: IllegalArgumentException) {}
    println("PASS: production Android AWG3 field adapter -> builder -> UAPI, base64-to-hex key, all timing ranges, legacy profiles and invalid input")
}
