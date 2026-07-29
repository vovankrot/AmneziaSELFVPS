# Fork-owned prebuilt binaries

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

## Still missing from a clean clone

These are also fork-only and still live in the submodule working tree, so a fresh
clone will not have them:

- `deploy-prebuilt/windows/x64/hysteria/hysteria.exe` (~21 MB)
- `deploy-prebuilt/windows/x64/anytls/anytls-client.exe` (~6 MB)
- `3rd-prebuilt/openssl/lib/libcrypto.lib`, `libssl.lib` (~38 MB, link-time only)

Moving the first two here would make the Windows installer fully reproducible
from a clean clone. The OpenSSL libs are a separate problem: they are needed at
link time, before staging runs, so this mechanism does not cover them.
