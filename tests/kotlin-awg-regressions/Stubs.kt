package org.amnezia.vpn.protocol

// Android/system services are substituted; the production AWG field adapter and
// WireguardConfig builder/serialization run unchanged on the JVM.
open class ProtocolConfig(builder: Builder) {
    val routes = builder.routes
    data class Route(val include: Boolean, val inetNetwork: String)
    open class Builder(allowSplitTunneling: Boolean) {
        val routes = mutableListOf(Route(true, "0.0.0.0/0"))
        open var mtu = 1280
        protected fun configBuild() = this
        open fun build(): ProtocolConfig = ProtocolConfig(this)
    }
}
class BadConfigException(message: String) : Exception(message)
