"""Record reviewed binaries. Run deliberately after changing a dependency.
Verification never calls this generator automatically.
"""
from pathlib import Path
import hashlib, json, re, subprocess
root=Path(__file__).resolve().parents[1]
entries=[]
submodule=subprocess.check_output(['git','-C',str(root/'client/3rd-prebuilt'),'rev-parse','HEAD'],text=True).strip()
for directory in ['deploy/prebuilt-selfvps/windows/x64','deploy/prebuilt-selfvps/android','client/3rd-prebuilt/deploy-prebuilt/windows/x64']:
    for path in sorted((root/directory).rglob('*')):
        if not path.is_file() or path.suffix not in ('.exe','.dll','.sys','.so','.inf','.cat'): continue
        data=path.read_bytes(); digest=hashlib.sha256(data).hexdigest()
        rel=path.relative_to(root).as_posix()
        own=directory.startswith('deploy/')
        toolchain=re.search(rb'go1\.\d+\.\d+',data)
        source='https://github.com/amnezia-vpn/3rd-prebuilt'
        revision=submodule; license='See upstream source and deploy/license'
        if own:
            if path.name.startswith('mullvad-split-tunnel.'):
                source='https://github.com/mullvad/mullvadvpn-app-binaries/tree/main/x86_64-pc-windows-msvc/split-tunnel'; revision='1.3.0.0; source 0a0eb97f67d1dbcb3d08bda66d3b24f465d95475'; license='GPL-3.0 / MPL-2.0; bundled licenses/mullvad-split-tunnel'
            elif 'anytls/' in rel:
                source='https://github.com/anytls/anytls-go'; revision='2012ef89768409f45437f1c06a7af5f6eea402ad + tools/anytls-security/patch.py'; license='Upstream repository does not contain a LICENSE file at this revision; review before public distribution'
            elif 'hysteria/' in rel:
                source='https://github.com/apernet/hysteria'; revision='2.12.2 (619a6f856b69fb7ee6a7a379e810e68b84004605)'; license='MIT'
            elif rel.endswith('tunnel.dll'):
                source='https://github.com/amnezia-vpn/amneziawg-windows'; revision='3.0.2'; license='MIT'
            else:
                source='https://github.com/amnezia-vpn/amneziawg-android'; revision='3.0.1'; license='Apache-2.0 / bundled upstream licenses'
            path.with_name(path.name+'.sha256').write_text(digest+'\n')
        entries.append(dict(path=rel,sha256=digest,source=source,revision=revision,
            architecture='arm64-v8a' if '/android/' in rel else 'windows-x64',
            toolchain=toolchain.group().decode() if toolchain else 'Not embedded; inherited audited binary snapshot',license=license))
blobs={'sys':'a3b826300e48acc385cbc8fe94fb138b6cf4ca20','inf':'cb1339196acbcb02ceebfef7cb8a965c7deed8b8','cat':'7e8c568d76c493d0871a8864326e4c9d1093dfaa'}
for entry in entries:
    if entry['path'].startswith('deploy/') and Path(entry['path']).name.startswith('mullvad-split-tunnel.'):
        entry['upstreamGitBlobSha1']=blobs[Path(entry['path']).suffix[1:]]
(root/'deploy/prebuilt-selfvps/manifest.json').write_text(json.dumps({'formatVersion':1,'files':entries},indent=2)+'\n')
print(f'Recorded {len(entries)} files. Review source metadata before publishing.')
