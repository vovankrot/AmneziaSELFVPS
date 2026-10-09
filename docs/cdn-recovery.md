# Automatic recovery of an individual CDN destination

Status: **foundation and experimental routing control implemented and tested;
automatic recovery is not enabled in the application**.
The requested behaviour is a temporary exception for the failing destination,
not removing an entire application from the bypass list. Existing exclusions,
LAN access and the running VPN session must remain intact.

## Implemented foundation

`client/core/tunnelDataProbe.h` now supports an explicit numeric destination.
SOCKS CONNECT uses that address; certificate verification, SNI and HTTP Host
use the original hostname. Both controls can therefore test the same CDN node.
The direct control requires a socket preparation callback: merely selecting
`NoProxy` does not prove that traffic bypasses OS tunnel routes. A rejected or
missing physical-path setup is inconclusive, not a CDN timeout. Certificate
verification remains mandatory; probes request no tokens or response body.

`client/core/cdnRecoveryPolicy.h` owns the decision for one session and one
public IPv4 TCP/443 destination:

- Three direct timeouts with a successful VPN control are required, at least
  ten seconds apart. Inconclusive controls, certificate failures, successful
  direct responses and long gaps reset the failure streak.
- Installation requires acknowledgement from the routing backend. A proposed
  exception is not reported as an installed exception.
- Recovery requires three successful direct probes and at least two minutes
  of holding the exception. The maximum lease is ten minutes.
- Expiry and session shutdown request removal. Failed removal retains ownership
  for retry; shutdown during installation waits for its acknowledgement and
  then requests cleanup. Results from another session are ignored.
- Private, loopback, CGNAT, documentation, benchmarking and reserved addresses
  cannot become recovery targets. Other ports and IPv6 are not supported by
  this initial policy.

Transport regressions exercise numeric SOCKS addressing with a local HTTPS
server, SNI, Host, trusted/untrusted certificates, wrong hostnames, invalid path
setup, thresholds, time windows, lease expiry and failed cleanup. These tests
do not modify Windows routes, WFP filters or an installed VPN.

Validation on 2026-10-06: Release build of `transport-regressions` completed;
16 QtTest cases passed, zero failed. Results are saved locally in
`.local/cdn-recovery-test-results.txt`. No installer or production update was
created for this incomplete feature.

## Routing backend and experimental control

The shipped signed Mullvad split driver exposes application paths and adapter
addresses, not destination exceptions. Its TCP connect callout rewrites
excluded applications to the physical source address. Its auth callout also
blocks excluded applications on the tunnel. A `/32` tunnel route alone cannot
override both mechanisms.

The upstream source recorded with the binary is
https://github.com/mullvad/win-split-tunnel/tree/0a0eb97f67d1dbcb3d08bda66d3b24f465d95475.
Both callout filters use the maximum UINT64 weight in the supplied baseline
sublayer. Adding another filter at the same weight is not a reliable priority
mechanism. Modifying the driver's dynamic-session filters from the service's
different WFP session is also not supported:
https://learn.microsoft.com/en-us/windows/win32/api/fwpmu/nf-fwpmu-fwpmfilterdeletebykey0.

An experimental control now uses a dedicated recovery sublayer above a separate,
lower-priority baseline supplied to the existing signed driver at initialization.
It is compiled only with `AMNEZIA_EXPERIMENTAL_CDN_RECOVERY=ON`; normal builds
default to OFF. Three exact executable/IPv4/TCP443 filters suppress source
rewriting and permit tunnel authorization only with the selected TUN source.
Installation/removal are transactional and owned by a dynamic WFP session.
Failed removal retains ownership for retry. No OS route is changed: this initial
control refuses unless the existing selected route and source are the TUN.
It must be tested with site split disabled; XRay direct-outbound rules cannot
be detected through the OS route table.

All foreign blocking-capable policies must precede the test recovery layer;
an unknown/lower or equal policy causes refusal. Static permits and inspection-
only callouts cannot deny the connection and are not treated as blocking policy.
The tested driver/recovery weights are 1 and 2. A read of this machine's WFP
layout found its lowest blocking-capable foreign weight to be 3; Kaspersky's
observed weight was 200. These are observations of this machine, not assumed
constants for other machines. Changing the baseline still requires controlled
live validation and monitoring of provider changes before production use.

On 2026-10-08 the real Windows WFP engine accepted all three filter definitions
inside a transaction which was then aborted; no traffic filters were applied.
The experimental and normal service variants built, and 10 QtTest results
passed (including setup/cleanup test hooks). Current installed binaries and
the active VPN were not changed. This validates API/schema and failure handling,
not packet egress or a live Riot fix. A separate 60-second control and guarded
service Stage/Restore script are in `tools/cdn-recovery-control` / its test kit.
The live control requires a maintenance window to change driver initialization;
its result is still pending. Automatic discovery, production IPC integration,
XRay dynamic rules and UI are not implemented by this control.

Additional integration requirements:

- Discover destinations from observed application failures and a fresh DNS
  snapshot; do not hardcode Riot addresses or silently probe arbitrary targets.
- Force the direct control onto the captured physical interface; ensure the
  VPN control really uses a proxy outbound rather than an XRay direct rule.
- Keep recovery exceptions ahead of site/geo direct rules, with bounded state
  and DNS expiry. Do not restart XRay merely to update a rule.
- Address HTTP/3 separately. UDP bind rewriting cannot be treated as TCP
  connect rewriting. Do not advertise this initial policy as QUIC recovery.
- Display the actual destination exception and expiry in the existing UI.
  An IP exception also affects other hostnames sharing that address.
- Validate packet source, egress, exclusion preservation, LAN, reconnect,
  service/client crash cleanup and failed WFP operations on the real backend.

An HTTPS HEAD response proves that the selected transport can return HTTP data;
it does not prove that a particular media segment or sustained transfer works.
Automatic detection of download stalls still needs observed flow evidence.
The root cause of the previously observed Riot destination timeout remains
unresolved; these changes do not establish that the driver, ISP or CDN caused it.

## Pre-connect DNS and route repair (2026-10-08)

Windows XRay/SSXray site rules now prepare a fresh snapshot of all returned
IPv4 addresses for each exact domain before starting the protocol. Preparation
is asynchronous, deduplicates hostnames, has at most 16 outstanding lookups
and a single 10-second deadline. A failed domain retains its own saved address;
a successful lookup replaces that address only in the current session's config.
Saved settings are not overwritten. Explicit IPs, CIDRs and wildcard patterns
are not resolved; wildcard rules no longer inherit an unrelated cached IP.

Cancellation, disconnect, shutdown, a changed session or edited site settings
prevent an obsolete result from starting a protocol. AWG, Hysteria2 and AnyTLS
keep their existing supported routing behavior and do not wait for this stage.

Windows route monitoring also recognizes AF_UNSPEC next hops as on-link routes
when LAN access is allowed. An exclusion is registered before captured routes
are reconciled, so a competing tunnel clone is removed in the same pass.
These changes adapt the ideas in official PRs #3030/#2825 and #2835 to this fork;
they are not a merge of those proposals or the per-CDN recovery backend.

Regression tests cover multi-address replies, cached fallbacks, bounded lookup
parallelism, a deadline, stale replies, settings changes, literal and wildcard
rules, and preservation of on-link LAN routes. A build/test pass does not prove
that Riot's physical-path TLS timeout or a sustained video download is fixed.
