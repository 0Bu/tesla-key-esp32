"""Actual Pages builder changelog admission controls in owned offline Git fixtures."""
from pathlib import Path
import json,os,shutil,subprocess,tempfile,hashlib,datetime
R=Path(__file__).resolve().parents[1]

def main() -> None:
    rows=[]
    for mode in ['candidate','missing-generator','generator-delegation-omission','wrong-generated-version','validation-omission','restored']:
     with tempfile.TemporaryDirectory(prefix='f107-assembler-offline-') as directory:
      root=Path(directory);repo=root/'repo';repo.mkdir();(repo/'scripts').mkdir()
      for n in ['build-pages.sh','verify-vendored-esptool-js.sh','check-pages-manifest.py','check-otadata-contract.py','generate-ota-changelog.py']:
       shutil.copy2(R/'scripts'/n,repo/'scripts'/n)
      shutil.copytree(R/'docs',repo/'docs');shutil.copy2(R/'LICENSE',repo/'LICENSE')
      env={k:v for k,v in os.environ.items() if not k.startswith(('GIT_','GH_','GITHUB_','OTA_SIGNING_'))}
      def git(*args):
       return subprocess.run(['git','-c','core.hooksPath=/dev/null','-c','core.fsmonitor=false','-c','commit.gpgsign=false',*args],cwd=repo,env=env,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE).stdout.decode().strip()
      git('init','--template=','-q');git('-c','user.name=Fixture','-c','user.email=fixture@example.invalid','commit','--allow-empty','-qm','fix: Retain bounded release metadata')
      sha=git('rev-parse','HEAD');stage=repo/'_fw';stage.mkdir()
      for target in ('esp32','esp32s3','esp32c3','esp32c6'):
       d=stage/target;d.mkdir()
       for name,data in [('bootloader.bin',b'boot'),('partition-table.bin',b'table'),('tesla-key-esp32.bin',b'fixture-not-a-flashable-image'),('ota_data_initial.bin',b'\xff'*8192)]: (d/name).write_bytes(data)
      builder=repo/'scripts/build-pages.sh';source=builder.read_text()
      generator=repo/'scripts/generate-ota-changelog.py'
      if mode in ('missing-generator','generator-delegation-omission'):generator.unlink()
      if mode=='generator-delegation-omission':
       start=source.index('if [[ ! -f "$repo_root/scripts/generate-ota-changelog.py"')
       end=source.index('\necho "Built verified Pages site',start)
       source=source[:start]+source[end:]
      if mode in ('wrong-generated-version','validation-omission'):
       real=repo/'scripts/generator-real.py';shutil.copy2(generator,real)
       generator.write_text('import sys,json,runpy\nfrom pathlib import Path\nif "--validate" in sys.argv: runpy.run_path(str(Path(__file__).with_name("generator-real.py")),run_name="__main__")\nelse: Path(sys.argv[sys.argv.index("--output")+1]).write_text(json.dumps({"version":"9.9.9","changelog":"Wrong generated version."}))\n')
      if mode=='validation-omission':
       needle='python3 "$repo_root/scripts/generate-ota-changelog.py" '+chr(92)+'\n  --validate "$out/changelog.json" --version "$version"'
       assert source.count(needle)==1;source=source.replace(needle,': # valid omission of final validation')
      builder.write_text(source)
      result=subprocess.run(['/bin/bash',str(builder),str(repo/'_site'),'1.2.3',sha],cwd=repo,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=30)
      expect_ok=mode in ('candidate','generator-delegation-omission','validation-omission','restored')
      assert (result.returncode==0)==expect_ok,(mode,result.returncode,result.stderr.decode())
      output=repo/'_site/changelog.json'
      oracle=result.returncode==0 and output.is_file() and json.loads(output.read_text()).get('version')=='1.2.3'
      assert oracle==(mode in ('candidate','restored')),(mode,oracle)
      rows.append({'case':mode,'exit':result.returncode,'completeMetadataOracle':oracle,'expectedSemanticOmissionDetected':mode.endswith('omission'),'stdoutSha256':hashlib.sha256(result.stdout).hexdigest(),'stderrSha256':hashlib.sha256(result.stderr).hexdigest()})
    receipt={'created':datetime.datetime.now(datetime.timezone.utc).isoformat(),'result':'PASS','rows':rows,'scope':'Actual complete Pages shell and actual generator/validator in disposable local Git fixtures with expressly synthetic non-flashable firmware input. No remote/network/key/signature/sourcepublication or live action. Two effective delegation omissions make metadata oracle fail while shell succeeds.'}
    print('Actual Pages changelog assembler:',len(rows),'PASS')


if __name__ == "__main__":
    main()
