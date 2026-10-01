# SELFVPS AnyTLS certificate verification

Base: anytls/anytls-go v0.0.12, commit `2012ef89768409f45437f1c06a7af5f6eea402ad`.
The sample upstream client disables all certificate verification. SELFVPS requires
an exact SHA-256 pin of the leaf certificate and TLS 1.2 or later. The certificate
is fetched over a host-key-verified SSH session. No insecure fallback is supported.
Server certificate/key files persist across restarts. Existing servers using the
upstream ephemeral identity must be reinstalled/updated in the test VM before
generating a new AnyTLS profile.

The Windows client receives its password through `ANYTLS_PASSWORD`, avoiding
the process command line. Do not enable `TLS_KEY_LOG` in production.

Rebuild without installing anything:

1. Clone upstream at the commit above into an isolated directory.
2. `python tools/anytls-security/patch.py <checkout>` (one-time, fails on unexpected source).
3. In that checkout: `go test ./util ./cmd/client ./cmd/server`.
4. `go build -trimpath -o anytls-client.exe ./cmd/client` (Windows amd64, CGO disabled).
5. Copy the binary to `deploy/prebuilt-selfvps/windows/x64/anytls/anytls-client.exe`
   and update the prebuilt manifest with its actual SHA-256.
6. `python tools/anytls-security/generate-dockerfile.py` embeds the same patch
   for the VPS multi-stage build. It performs the certificate handshake tests.

The pin tests use a real TLS server: the correct pin succeeds despite the
masquerade name, an incorrect/missing pin fails. End-to-end VPS networking must
also be checked after installation on the VM. Docker base images are pinned by
their multi-platform registry manifest digest; Alpine package repository updates
still require a deliberate dependency review.
