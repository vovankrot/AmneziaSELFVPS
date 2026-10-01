"""Apply the SELFVPS certificate-verification patch to anytls-go v0.0.12.

The upstream checkout must be commit 2012ef89768409f45437f1c06a7af5f6eea402ad.
No installation or service changes are performed by this script.
"""
import sys
from pathlib import Path

PIN_TLS = '''package util

import (
    "crypto/sha256"
    "crypto/subtle"
    "crypto/tls"
    "encoding/hex"
    "errors"
    "strings"
)

// Self-hosted servers use a certificate obtained through authenticated SSH.
// PKI name validation is replaced by exact leaf-certificate pin validation.
func PinnedTLSConfig(serverName, pin string) (*tls.Config, error) {
    expected, err := hex.DecodeString(strings.ReplaceAll(pin, ":", ""))
    if err != nil || len(expected) != sha256.Size {
        return nil, errors.New("a valid SHA-256 server certificate pin is required")
    }
    return &tls.Config{
        ServerName: serverName, MinVersion: tls.VersionTLS12,
        InsecureSkipVerify: true,
        VerifyConnection: func(state tls.ConnectionState) error {
            if len(state.PeerCertificates) == 0 { return errors.New("missing peer certificate") }
            actual := sha256.Sum256(state.PeerCertificates[0].Raw)
            if subtle.ConstantTimeCompare(actual[:], expected) != 1 {
                return errors.New("server certificate pin mismatch")
            }
            return nil
        },
    }, nil
}
'''

PIN_TEST = '''package util
import (
    "crypto/sha256"
    "crypto/tls"
    "encoding/hex"
    "net/http"
    "net/http/httptest"
    "testing"
)
func TestPinnedHandshake(t *testing.T) {
    server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.Write([]byte("ok")) }))
    defer server.Close()
    sum := sha256.Sum256(server.Certificate().Raw)
    cfg, err := PinnedTLSConfig("masquerade.example", hex.EncodeToString(sum[:]))
    if err != nil { t.Fatal(err) }
    transport := &http.Transport{TLSClientConfig: cfg}
    defer transport.CloseIdleConnections()
    client := &http.Client{Transport: transport}
    response, err := client.Get(server.URL)
    if err != nil { t.Fatal(err) }; response.Body.Close()
    bad := sha256.Sum256([]byte("wrong certificate"))
    cfg, _ = PinnedTLSConfig("masquerade.example", hex.EncodeToString(bad[:]))
    badTransport := &http.Transport{TLSClientConfig: cfg}
    defer badTransport.CloseIdleConnections()
    if response, err = (&http.Client{Transport: badTransport}).Get(server.URL); err == nil {
        response.Body.Close(); t.Fatal("incorrect certificate pin was accepted")
    }
    if _, err := PinnedTLSConfig("", ""); err == nil { t.Fatal("missing pin was accepted") }
    if cfg.MinVersion != tls.VersionTLS12 { t.Fatal("TLS minimum changed") }
}
'''

REPLACEMENTS = {
    'cmd/client/main.go': [
        ('\n\t"strings"', '\n\t"time"'),
        ('password := flag.String("p", "", "Password")', 'password := flag.String("p", os.Getenv("ANYTLS_PASSWORD"), "password (prefer ANYTLS_PASSWORD environment)")\n\tcertificatePin := flag.String("pin", "", "SHA-256 leaf certificate pin")'),
        ('*password = serverURL.User.String()', '*password = serverURL.User.Username()'),
        ('tlsConfig := &tls.Config{\n\t\tServerName:         *sni,\n\t\tInsecureSkipVerify: true,\n\t}', 'tlsConfig, err := util.PinnedTLSConfig(*sni, *certificatePin)\n\tif err != nil { logrus.Fatalln(err) }'),
        ('listener, err := net.Listen("tcp", *listen)\n\tif err != nil {\n\t\tlogrus.Fatalln("listen socks5 tcp:", err)\n\t}', ''),
        ('path := strings.TrimSpace(os.Getenv("TLS_KEY_LOG"))\n\tif path != "" {\n\t\tf, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR|os.O_APPEND, 0644)\n\t\tif err == nil {\n\t\t\ttlsConfig.KeyLogWriter = f\n\t\t}\n\t}', '''// Verify server identity before exposing a successful local SOCKS listener.
    probe, err := tls.DialWithDialer(&net.Dialer{Timeout: 4*time.Second}, "tcp", *serverAddr, tlsConfig)
    if err != nil { logrus.Fatalln("server TLS verification:", err) }
    probe.Close()
    listener, err := net.Listen("tcp", *listen)
    if err != nil { logrus.Fatalln("listen socks5 tcp:", err) }'''),
        ('conn, err := proxy.SystemDialer.DialContext(ctx, "tcp", *serverAddr)', 'handshakeCtx, cancel := context.WithTimeout(ctx, 8*time.Second)\n\t\tdefer cancel()\n\t\tconn, err := proxy.SystemDialer.DialContext(handshakeCtx, "tcp", *serverAddr)'),
        ('conn = tls.Client(conn, tlsConfig)\n\t\treturn conn, nil', '''tlsConn := tls.Client(conn, tlsConfig)
        if err := tlsConn.HandshakeContext(handshakeCtx); err != nil {
            conn.Close()
            return nil, err
        }
        return tlsConn, nil'''),
    ],
    'cmd/server/main.go': [
        ('\n\t"time"', ''),
        ('paddingScheme := flag.String', 'certPath := flag.String("cert", "", "persistent PEM certificate")\n\tkeyPath := flag.String("key", "", "persistent PEM key")\n\tpaddingScheme := flag.String'),
        ('tlsCert, _ := util.GenerateKeyPair(time.Now, "")', 'tlsCert, err := tls.LoadX509KeyPair(*certPath, *keyPath)\n\tif err != nil { logrus.Fatalln("persistent TLS identity:", err) }'),
        ('return tlsCert, nil', 'return &tlsCert, nil'),
        ('tlsConfig := &tls.Config{', 'tlsConfig := &tls.Config{\n\t\tMinVersion: tls.VersionTLS12,'),
    ],
}
def apply(source):
    for name, replacements in REPLACEMENTS.items():
        path=source/name
        data=path.read_text()
        for old,new in replacements:
            if data.count(old) != 1: raise RuntimeError(f'Unexpected upstream source: {name}: {old}')
            data=data.replace(old,new)
        path.write_text(data)
    (source/'util/pinned_tls.go').write_text(PIN_TLS)
    (source/'util/pinned_tls_test.go').write_text(PIN_TEST)

if __name__ == '__main__':
    apply(Path(sys.argv[1]))
