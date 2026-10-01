"""Real AnyTLS TLS/SOCKS echo and wrong-pin rejection, using loopback only.
Pass the test-only server executable built from tools/anytls-security/patch.py.
"""
import datetime, os, socket, subprocess, sys, tempfile, threading, time
from pathlib import Path
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID
root=Path(__file__).resolve().parents[1]
client=root/'deploy/prebuilt-selfvps/windows/x64/anytls/anytls-client.exe'
server=Path(sys.argv[1]).resolve()
def freeport():
    with socket.socket() as sock: sock.bind(('127.0.0.1',0)); return sock.getsockname()[1]
with tempfile.TemporaryDirectory(prefix='selfvps-anytls-test-') as temp:
    folder=Path(temp); key=ec.generate_private_key(ec.SECP256R1())
    name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'localhost')]); now=datetime.datetime.now(datetime.timezone.utc)
    cert=(x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
          .serial_number(x509.random_serial_number()).not_valid_before(now-datetime.timedelta(minutes=1))
          .not_valid_after(now+datetime.timedelta(days=1)).sign(key,hashes.SHA256()))
    (folder/'cert.pem').write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    (folder/'key.pem').write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    for valid in [True,False]:
        processes=[]; remoteport=freeport(); socksport=freeport(); reached=threading.Event()
        with socket.socket() as echo, (folder/'server.log').open('w') as serverlog, (folder/'client.log').open('w') as clientlog:
            echo.bind(('127.0.0.1',0));echo.listen();echo.settimeout(10);target=echo.getsockname()[1]
            def serve():
                try:
                    conn,_=echo.accept(); reached.set()
                    with conn: conn.sendall(conn.recv(4096))
                except OSError: pass
            worker=threading.Thread(target=serve,daemon=True); worker.start()
            try:
                processes.append(subprocess.Popen([str(server),'-l',f'127.0.0.1:{remoteport}','-p','test-only','-cert',str(folder/'cert.pem'),'-key',str(folder/'key.pem')],stdout=serverlog,stderr=serverlog))
                time.sleep(.2)
                pin=cert.fingerprint(hashes.SHA256()).hex() if valid else '00'*32
                env={k.upper():v for k,v in os.environ.items()};env['ANYTLS_PASSWORD']='test-only'
                processes.append(subprocess.Popen([str(client),'-l',f'127.0.0.1:{socksport}','-s',f'127.0.0.1:{remoteport}','-sni','masquerade.example','-pin',pin,'-m','0'],env=env,stdout=clientlog,stderr=clientlog))
                if not valid:
                    assert processes[-1].wait(6)!=0, 'Wrong pin was accepted at startup'
                    assert not reached.is_set(), 'Wrong-pin connection reached the target'
                    clientlog.flush();log=(folder/'client.log').read_text().lower();assert 'pin mismatch' in log,log
                    print('PASS: AnyTLS wrong certificate pin rejected before SOCKS listener starts')
                    continue
                stream=None
                for _ in range(40):
                    try: stream=socket.create_connection(('127.0.0.1',socksport),.2);break
                    except OSError: time.sleep(.1)
                assert stream, 'AnyTLS SOCKS listener did not start'
                with stream:
                    stream.settimeout(6)
                    def receive(n):
                        data=b''
                        while len(data)<n:
                            block=stream.recv(n-len(data))
                            if not block:break
                            data+=block
                        return data
                    stream.sendall(b'\x05\x01\x00');assert receive(2)==b'\x05\x00'
                    stream.sendall(b'\x05\x01\x00\x01'+socket.inet_aton('127.0.0.1')+target.to_bytes(2,'big'))
                    reply=receive(4)
                    if valid:
                        assert reply[:2]==b'\x05\x00',reply
                        receive({1:4,4:16}[reply[3]]+2)
                        payload=b'SELFVPS authenticated AnyTLS echo';stream.sendall(payload);assert receive(len(payload))==payload
                        assert reached.wait(1)
                        print('PASS: AnyTLS real TLS/SOCKS echo with correct certificate pin')
                    else:
                        assert reply[:2]!=b'\x05\x00', 'Wrong pin established an upstream connection'
                        assert not reached.is_set(), 'Wrong-pin connection reached the target'
                        clientlog.flush();log=(folder/'client.log').read_text().lower();assert 'pin mismatch' in log,log
                        print('PASS: AnyTLS wrong certificate pin blocks target connection')
            finally:
                for process in reversed(processes):
                    process.terminate()
                    try:process.wait(3)
                    except subprocess.TimeoutExpired:process.kill();process.wait()
