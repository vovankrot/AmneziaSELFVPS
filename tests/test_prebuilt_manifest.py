"""Fault-inject the actual packaging verifier in an isolated workspace."""
import hashlib, json, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[1]
script=root/'tools/verify-prebuilts.ps1'
with tempfile.TemporaryDirectory(prefix='selfvps-manifest-') as temp:
    folder=Path(temp); binary=folder/'deploy/prebuilt-selfvps/windows/x64/example.exe'
    binary.parent.mkdir(parents=True); binary.write_bytes(b'known binary snapshot')
    manifest=folder/'manifest.json'
    entry={'path':binary.relative_to(folder).as_posix(),'sha256':hashlib.sha256(binary.read_bytes()).hexdigest()}
    def verify():
        return subprocess.run(['powershell','-NoProfile','-File',str(script),'-Root',str(folder),'-ManifestPath',str(manifest)],capture_output=True,text=True)
    def save(record): manifest.write_text(json.dumps({'formatVersion':1,'files':[record]}))
    save(entry); result=verify(); assert result.returncode==0, result.stdout+result.stderr
    binary.write_bytes(b'changed'); assert verify().returncode!=0
    binary.write_bytes(b'known binary snapshot')
    extra=binary.with_name('unreviewed.dll'); extra.write_bytes(b'extra'); assert verify().returncode!=0; extra.unlink()
    binary.unlink(); assert verify().returncode!=0
    save({'path':'../outside.exe','sha256':'0'*64}); assert verify().returncode!=0
print('PASS: manifest accepts exact files; rejects altered, missing, unrecorded and escaping files')
