These tests compile the IO timeout helper extracted directly from the production
source. The OS call is replaced with a controllable delayed response. No VPN
driver or network configuration is touched.

Build with CMake/MSVC and run driver-regressions.exe. Tests cover bounded timeout,
quarantine, late completion after caller storage changes, valid output copying,
output length validation and preservation of the Windows error code.

They do not prove recovery of an already wedged kernel driver. That can require
a Windows restart; the service must report failure rather than claim recovery.

LAN policy tests compile the production transaction with WFP test doubles.
Failures at transaction start, rule insertion and commit must preserve the
previous policy. These tests do not prove real kernel WFP behavior.

Firewall transaction tests compile the exact enableInterface, allowTrafficRange,
and enablePeerTraffic methods plus their KillSwitch callers. WFP calls and rule
helpers are injected. A pending/committed policy model verifies preservation of
prior LAN and other-peer filters at begin failure, every individual insertion,
commit failure and deletion failure during replacement. Retrying must succeed.
The production IPAddress parser checks IPv4/IPv6 CIDR preservation. Range state
must change only after successful installation; caller failures must propagate.
These tests do not modify the installed firewall or prove live kernel rollback.

Peer deletion, full policy deletion and replacement of IPv6 app-bypass rules
also compile the exact production methods. Begin, each deletion/addition and
commit failures retain existing filter ownership. Strict Kill Switch changes
preserve active DNS/app rules; rejected preference writes are reported.

Network lifecycle tests inject Windows calls into production driver stop,
lazy driver activation, AWG default-route setup and native route capture.
They verify full monitor reset, invalid/quarantined states, read/write failures,
no capture before a successful default route, preservation of private and
public on-link LANs, and retained route ownership after failed removal.
Autoconnect waits for service readiness and cancels after a manual connection
or changed preference. These fixtures never load a driver or change OS routes.

Native cleanup tests execute the production controller stop/error/reply methods
and Daemon::deactivate. They check delayed acknowledgement, lost IPC, cleanup
retry after reconnect, and failures restoring DNS, deleting routes/peers,
stopping the interface and running the Down hook. Failed cleanup retains the
session ledger and never emits a successful disconnection.

Tunnel service tests execute production UAPI IO and stop methods with Windows
calls injected. Partial writes, asynchronous completion, timeout cancellation
and draining, empty/truncated replies, failed service stop and repeated cleanup
are covered. A service that never answers cannot cause an unbounded read.
Strict baseline plus ranges commit atomically; repeated range/peer replacement
cannot accumulate permits, and failure retains the prior policy.

Native policy tests compile production peer cleanup and Windows DNS methods.
They verify cleanup after a confirmed service crash, rejection of contradictory
UAPI replies, retained DNS ownership after partial failure, release after
success, per-command netsh failures/timeouts/crashes, and validation before DNS
is cleared. Service status query failures cannot prove that a tunnel stopped.
The controller tests also cover queued activation, cancellation by stop and
malformed initial service status. Daemon cleanup must not delete a peer twice
when a later cleanup step failed.

DHCP policy tests compile the production allowDHCPTraffic method. They check
UDP ports 68/67 for IPv4 and 546/547 for IPv6 at both ALE directions,
and propagate insertion failures at each of the four filters.

Live-log regressions cover duplicate endpoint exclusions, absent route code 2,
retained ownership on access denial, empty ring-log paths, unsigned ring index
wrap and a fixed finite polling snapshot while the log writer advances.

Exclusion gateway tests compile WindowsDaemon::getDefaultGateway with injected
Windows table reads. They cover combined route/interface metrics, VPN exclusion,
disconnected/expired/on-link/loopback entries, read failure, deterministic table
order, and clearing output values when no usable gateway remains.

Site/geo exclusion ownership tests compile the production daemon route methods
with injected WinAPI operations (no live route changes). They cover gateway or
interface changes, retained state and retry on deletion failure, deferring a
replacement while cleanup fails, leaving pre-existing rows alone, shared
site/geo references in either removal order, canonical CIDRs, and absent rows.

Address refresh regressions compile the production start, address collection,
registration and notification methods with injected Windows API reads and IOCTL.
They cover renewal from a callback thread, unchanged-address suppression,
network loss/recovery, link-local and tentative IPv6 rejection, route/interface
metrics, stale notifications after stop or a new session, bounded IOCTL/DAD
retries, quarantined handles, partial subscription rollback and failed startup.
No real network routes, driver subscriptions or IOCTL are used.
