# Fork-owned prebuilt binaries

> ⚠️ **`windows/x64/tunnel.dll` is temporarily a DEBUG build**, not the release one.
> It logs the exact UAPI configuration string the client sends to the AmneziaWG
> daemon (jc/jmin/jmax/s1-s4/h1-h4/header_protection_key/...; `private_key` is
> redacted) to the service log, to diagnose an AWG3 handshake that never
> completes despite matching keys on both ends -- see the "AWG3 diagnostics"
> section below. Once the header-protection issue is understood, rebuild a clean
> `tunnel.dll` with the "Rebuilding tunnel.dll" recipe below (same source, no
> debug patch) and drop it back in here.

Binaries that this fork ships but upstream's `client/3rd-prebuilt` submodule does
not carry. Everything here is layered **on top of** the submodule during staging,
so the submodule itself stays pristine and can be updated from upstream without
conflicts.

`build_installer.ps1` copies the submodule prebuilts first, then overwrites with
whatever is found here (step 5, "Copying prebuilt binaries"). Layout mirrors
`client/3rd-prebuilt/deploy-prebuilt/`, so `windows/x64/foo.exe` lands next to the
client executable.

## Why this exists

`client/3rd-prebuilt` is a submodule pointing at Amnezia's repository. Anything
dropped into its working tree lives only on the machine that put it there — it
cannot be committed to this repository, so a fresh `git clone` produces a build
missing those files. Keeping fork binaries here instead makes the repository
self-contained.

## Contents

| File | Source | Why it is here |
|------|--------|----------------|
| `windows/x64/tunnel.dll` | [`amneziawg-windows`](https://github.com/amnezia-vpn/amneziawg-windows) v3.0.2 | AmneziaWG 3 support. The submodule's copy is an April 2026 build that predates AWG3 and **rejects** a config containing `HeaderProtectionKey` or `ContentPaddingAddition` outright, killing the tunnel rather than ignoring the keys. |
| `windows/x64/hysteria/hysteria.exe` | Hysteria 2 | Protocol this fork added; never existed upstream. |
| `windows/x64/anytls/anytls-client.exe` | AnyTLS | Protocol this fork added; never existed upstream. |
| `android/arm64-v8a/libwg-go.so`, `libwg.so`, `libwg-quick.so` | [`amneziawg-android`](https://github.com/amnezia-vpn/amneziawg-android) v3.0.1, NDK r27c | AmneziaWG 3 for Android. Same story as `tunnel.dll`: the submodule's build has no AWG3 support. `client/cmake/android.cmake` prefers these and warns when falling back. |

## Rebuilding tunnel.dll

Needs Go (1.25+, or any Go with `GOTOOLCHAIN=auto` so it can fetch one) and a
MinGW toolchain. Same command upstream's own Conan recipe uses:

```bash
curl -L -o awg.zip https://github.com/amnezia-vpn/amneziawg-windows/archive/refs/tags/v3.0.2.zip
# Verify against the sha256 pinned in upstream's recipes/awg-windows/conanfile.py:
#   e5755ef1e19fd8408881cab49684d37ee4a0822d706960bbabe89770f7c436f1
sha256sum awg.zip
unzip -q awg.zip && cd amneziawg-windows-3.0.2

export GOOS=windows GOARCH=amd64 CGO_ENABLED=1
export CC=/path/to/mingw64/bin/gcc.exe
export CGO_CFLAGS="-Wall -Wno-unused-function -Wno-switch -DWINVER=0x0601"
export CGO_LDFLAGS="-Wl,--dynamicbase -Wl,--nxcompat -Wl,--export-all-symbols -Wl,--high-entropy-va"

go build -buildmode c-shared -ldflags="-w -s" -trimpath -o tunnel.dll .
```

Sanity-check the result actually speaks AWG3 before shipping it — these strings
are absent from any pre-AWG3 build:

```bash
strings -a tunnel.dll | grep -E "header_protection_key|content_padding_addition"
```

Then drop it in `windows/x64/` and refresh `tunnel.dll.sha256`.

## Rebuilding the Android natives

Needs a Linux host (the `libwg-go` Makefile only knows Go hashes for
`linux-amd64` and macOS) plus the Android NDK r27c and CMake. WSL works. No Go
needed up front — the Makefile downloads and patches its own.

```bash
git clone --depth 1 --recurse-submodules --branch v3.0.1 \
    https://github.com/amnezia-vpn/amneziawg-android.git
cmake -S amneziawg-android/tunnel/tools -B build \
    -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
    -DANDROID_PACKAGE_NAME=org.amnezia.vpn \
    -DGRADLE_USER_HOME=build/gradle_user_home \
    -DCMAKE_LIBRARY_OUTPUT_DIRECTORY=build/out
cmake --build build --target libwg-go.so libwg.so libwg-quick.so
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-unneeded build/out/*.so
```

`ANDROID_PACKAGE_NAME` is baked into the socket path inside `libwg-go.so`, so it
has to match the app's package.

> If you extract the NDK zip with `python3 -m zipfile`, **don't**. It silently
> drops symlinks — `bin/clang` becomes a text file containing `clang-18` — and
> CMake then reports "C compiler identification is unknown". Use `unzip`, or an
> extractor that handles symlinks and permission bits.

Verify the same way as the Windows build before shipping:

```bash
strings -a libwg-go.so | grep -E "header_protection_key|content_padding_addition"
```

Only `arm64-v8a` is built today, matching the APK this fork ships. The other ABIs
still fall back to the submodule's pre-AWG3 prebuilts, and `android.cmake` emits
a CMake warning when that happens.

## AWG3 diagnostics (temporary)

The client and server both apply AWG3 header protection with byte-identical
keys (verified via hexdump on both ends), S1-S4 well above the 12-byte nonce
minimum, and packets confirmed reaching the server (tcpdump) -- yet the
handshake never completes, silently, on both sides (which is by design for
AWG: DPI must not be able to tell valid traffic from garbage, so failed parses
are never logged). Static review of `amneziawg-go`'s header-protection crypto
(send.go/receive.go/noise-protocol.go) and its commit history between the
v3.0.1 tag this client is built against and the server's ~July 31 build found
no functional change to that code path -- only a cosmetic error-message fix.

This build adds one temporary debug line to `service.go` right after
`config.ToUAPI()`, logging every line of the UAPI string the client is about
to send to the daemon (`private_key` redacted, everything else -- including
`header_protection_key` -- left visible) into the same service log this
project already reads via ringlogger. The goal is a byte-exact comparison
against what `awg show` reports on the server, without guessing further from
source alone.

Once connected with this build, the client's `AmneziaVPN-service.log` should
contain a block bracketed by:

```
DEBUG uapiConf BEGIN
DEBUG uapi> jc=...
DEBUG uapi> ...
DEBUG uapi> header_protection_key=<hex>
DEBUG uapiConf END
```

Compare that against the server's `awg show` / `/opt/amnezia/awg/awg0.conf`.

## Still missing from a clean clone

- `3rd-prebuilt/openssl/lib/libcrypto.lib`, `libssl.lib` (~38 MB)

These are needed at **link** time, before staging runs, so this mechanism does not
cover them; a clean clone still cannot link the Windows client without them.
