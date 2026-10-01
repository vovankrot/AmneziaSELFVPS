"""Exercise production Kotlin serialization using an already available compiler.
This does not build/install an APK or validate JNI/device networking.
"""
from pathlib import Path
import argparse, os, subprocess
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--gradle-cache',default=str(Path(os.environ.get('USERPROFILE',str(Path.home())))/'.gradle/caches/modules-2/files-2.1'))
parser.add_argument('--java',default=str(Path(os.environ.get('ProgramFiles',r'C:/Program Files'))/'Java/jdk-21/bin/java.exe'))
options=parser.parse_args()
root=Path(__file__).resolve().parents[1]
cache=Path(options.gradle_cache)
java=Path(options.java)
def jar(module,version=None):
    paths=list((cache/module).rglob('*.jar'))
    if version: paths=[p for p in paths if version in p.parts]
    if not paths: raise RuntimeError(f'Existing compiler dependency not found: {module}')
    return paths[0]
compiler=jar('org.jetbrains.kotlin/kotlin-compiler-embeddable','2.0.20')
stdlib=jar('org.jetbrains.kotlin/kotlin-stdlib','2.0.20')
compilerpath=[compiler,stdlib,jar('org.jetbrains.kotlin/kotlin-script-runtime'),jar('org.jetbrains.kotlin/kotlin-reflect'),
    jar('org.jetbrains.intellij.deps/trove4j'),jar('org.jetbrains/annotations'),jar('org.jetbrains.kotlinx/kotlinx-coroutines-core-jvm')]
folder=root/'build-installer/kotlin-awg-regressions';folder.mkdir(parents=True,exist_ok=True)
source=root/'client/android/wireguard/src/main/kotlin/org/amnezia/vpn/protocol/wireguard/Wireguard.kt'
text=source.read_text();start=text.index('    protected fun WireguardConfig.Builder.configExtensionParameters(');end=text.index('\n    private fun start(',start)
adapter=folder/'ProductionAdapter.kt'
adapter.write_text('package org.amnezia.vpn.protocol.wireguard\nimport org.json.JSONObject\nimport org.amnezia.vpn.util.optStringOrNull\n'+text[start:end].replace('protected fun','fun',1))
sources=list((root/'tests/kotlin-awg-regressions').glob('*.kt'))+[source.with_name('WireguardConfig.kt'),adapter]
output=folder/'classes';output.mkdir(exist_ok=True)
env={k.upper():v for k,v in os.environ.items()}
args=[str(java),'-cp',os.pathsep.join(map(str,compilerpath)),'org.jetbrains.kotlin.cli.jvm.K2JVMCompiler',
    '-no-stdlib','-no-reflect','-language-version','1.9','-jvm-target','17','-classpath',str(stdlib)+';'+str(jar('org.jetbrains/annotations')),'-d',str(output),*map(str,sources)]
subprocess.run(args,env=env,check=True)
subprocess.run([str(java),'-cp',str(output)+';'+str(stdlib),'org.amnezia.vpn.protocol.wireguard.MainKt'],env=env,check=True)
