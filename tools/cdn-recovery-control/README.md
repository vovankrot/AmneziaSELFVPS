# Isolated CDN routing control (experimental)

This is a test kit, not a production installer or automatic recovery feature.
Do not replace the normal driver package: the existing signed driver is used.
The test service is compiled with `AMNEZIA_EXPERIMENTAL_CDN_RECOVERY=ON`.
The normal build defaults to `OFF` and retains its original driver baseline.

The kit's `switch-service.ps1` changes only AmneziaVPN-service.exe. It requires
administrator rights, refuses while the client is running, validates hashes,
saves the original binary and rolls back if the test service fails to start.
It does not kill processes, change SCM registration, delete settings or touch
driver files. Restore is a separate explicit operation using the saved binary.
It also requires closing AmneziaVPN. There is no automatic service replacement.

After an explicitly authorized maintenance window:

1. Disconnect VPN and close AmneziaVPN. Run `switch-service.ps1 -Mode Stage`
   as administrator, then open the client and connect using XRay. Site split
   must be off for this initial test; existing app exclusions stay enabled.
2. Run `control/cdn-wfp-control.exe --preflight` as administrator. It verifies
   the exact driver filters and refuses unknown/incompatible provider ordering.
3. In a separate administrator console, run the 60-second control, using the
   current TUN IPv4 and interface index (the example numbers are not permanent):

   ```powershell
   .\control\cdn-wfp-control.exe --apply "C:\Program Files\Mozilla Firefox\firefox.exe" 104.16.57.21 10.33.0.2 28
   ```

4. While active, test fresh connections to both Riot IPv4 addresses using an
   isolated Firefox profile. Check TLS, source address and physical/TUN egress;
   verify that the working address and LAN remain direct. Existing established
   connections are not migrated. The exception covers only this executable,
   this numeric destination and TCP/443. Hostnames sharing that IP are included.
5. The control deletes its three filters and sublayer at 60 seconds. A dynamic
   WFP session also owns cleanup if the control process exits or crashes.
   Removal failure is reported, not presented as successful cleanup.
6. Disconnect VPN, close the client, run `switch-service.ps1 -Mode Restore`
   as administrator, then open AmneziaVPN and reconnect. Restore verifies the
   original binary. If standard service stop fails, the script does not force
   termination or replace a running binary; investigate before continuing.

The control requires an already selected TUN route and makes no OS route
changes. It is not compatible with an XRay direct outbound rule for the target;
those rules cannot be detected from the OS route table. UDP/QUIC, IPv6,
automatic failed-flow discovery, XRay dynamic-rule updates, and the UI remain
outside this initial control. Provider changes while the control is active
still need monitoring before this mechanism can become a production feature.

WFP hard-permit arbitration and provider coexistence require live validation:
https://learn.microsoft.com/en-us/windows/win32/fwp/filter-arbitration.
An aborted transaction validates the conditions/actions, not actual routing.
