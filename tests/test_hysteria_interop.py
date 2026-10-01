"""Real QUIC + Salamander + SOCKS data transfer across old/new Hysteria versions.
Only loopback sockets and disposable test certificates/configurations are used.
"""
import datetime
import ipaddress
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

ROOT=Path(__file__).resolve().parents[1]
NEW=ROOT/'deploy/prebuilt-selfvps/windows/x64/hysteria/hysteria.exe'
OLD=ROOT/'client/3rd-prebuilt/deploy-prebuilt/windows/x64/hysteria/hysteria.exe'

def port(kind):
    with socket.socket(socket.AF_INET,kind) as s:
        s.bind(('127.0.0.1',0))
        return s.getsockname()[1]

with tempfile.TemporaryDirectory(prefix='selfvps-interop-') as temp:
    folder=Path(temp)
    key=ec.generate_private_key(ec.SECP256R1())
    name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'localhost')])
    now=datetime.datetime.now(datetime.timezone.utc)
    cert=(x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
          .serial_number(x509.random_serial_number()).not_valid_before(now-datetime.timedelta(minutes=1))
          .not_valid_after(now+datetime.timedelta(days=1))
          .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address('127.0.0.1'))]),False)
          .sign(key,hashes.SHA256()))
    (folder/'cert.pem').write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    (folder/'key.pem').write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    for server_bin,client_bin,label,valid_pin in [(NEW,NEW,'new/new',True),(NEW,OLD,'new server/old client',True),(OLD,NEW,'old server/new client',True),(NEW,NEW,'wrong certificate pin',False)]:
        udp=port(socket.SOCK_DGRAM); socks=port(socket.SOCK_STREAM)
        (folder/'server.yaml').write_text(f'''listen: 127.0.0.1:{udp}
tls:
  cert: cert.pem
  key: key.pem
auth:
  type: password
  password: test-only
obfs:
  type: salamander
  salamander:
    password: test-obfuscation
''')
        (folder/'client.yaml').write_text(f'''server: 127.0.0.1:{udp}
auth: test-only
tls:
  insecure: true
  pinSHA256: {cert.fingerprint(hashes.SHA256()).hex() if valid_pin else '00'*32}
obfs:
  type: salamander
  salamander:
    password: test-obfuscation
socks5:
  listen: 127.0.0.1:{socks}
''')
        processes=[]
        with socket.socket() as echo, (folder/'server.log').open('w') as server_log, (folder/'client.log').open('w') as client_log:
            echo.bind(('127.0.0.1',0)); echo.listen(); echo.settimeout(15)
            target=echo.getsockname()[1]
            def serve():
                conn,_=echo.accept()
                with conn: conn.sendall(conn.recv(4096))
            worker=threading.Thread(target=serve,daemon=True)
            if valid_pin: worker.start()
            try:
                for binary,mode,log in [(server_bin,'server',server_log),(client_bin,'client',client_log)]:
                    processes.append(subprocess.Popen([str(binary),mode,'--disable-update-check','-c',mode+'.yaml'],cwd=folder,stdout=log,stderr=log))
                    time.sleep(.3)
                if not valid_pin:
                    assert processes[-1].wait(8) != 0, 'Incorrect pin was accepted'
                    client_log.flush()
                    message=(folder/'client.log').read_text().lower()
                    assert 'certificate' in message or 'pin' in message, message
                    print('PASS: incorrect certificate pin rejected before SOCKS/data transfer')
                    continue
                connection=None
                for _ in range(60):
                    try: connection=socket.create_connection(('127.0.0.1',socks),.3); break
                    except OSError: time.sleep(.1)
                assert connection is not None, (folder/'client.log').read_text()
                with connection as stream:
                    stream.settimeout(5)
                    def receive(n):
                        data=b''
                        while len(data)<n:
                            chunk=stream.recv(n-len(data))
                            assert chunk, 'Unexpected EOF'
                            data+=chunk
                        return data
                    stream.sendall(b'\x05\x01\x00'); assert receive(2)==b'\x05\x00'
                    stream.sendall(b'\x05\x01\x00\x01'+socket.inet_aton('127.0.0.1')+target.to_bytes(2,'big'))
                    response=receive(4); assert response[:2]==b'\x05\x00',response
                    receive({1:4,4:16}[response[3]]+2)
                    payload=b'SELFVPS real Hysteria interop test'
                    stream.sendall(payload); assert receive(len(payload))==payload
                worker.join(2)
                print('PASS:',label,'SOCKS echo through QUIC/Salamander')
            finally:
                for process in reversed(processes):
                    process.terminate()
                    try: process.wait(3)
                    except subprocess.TimeoutExpired: process.kill(); process.wait()
