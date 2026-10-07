"""Real local archive extraction with the same native worker used on PS5."""
from pathlib import Path
import binascii,hashlib,io,json,subprocess,tarfile,tempfile,zipfile
ROOT=Path(__file__).resolve().parents[1]
DATA=bytes(range(256))*1024

def main():
 with tempfile.TemporaryDirectory(prefix="ps5-react-archives-") as tmp:
  root=Path(tmp);binary=root/"archive-client"
  prefix=subprocess.check_output(["brew","--prefix","libarchive"],text=True).strip()
  subprocess.run(["clang++","-std=c++20","-O2","-Wall","-Wextra","-Werror","-pthread","-I",str(ROOT/"native/shared"),"-I",prefix+"/include",str(ROOT/"tools/tests/archive_client.cpp"),str(ROOT/"native/shared/archives.cpp"),"-L",prefix+"/lib","-larchive","-o",str(binary)],check=True)
  def run(name,parts,limit=1024*1024):
   out=subprocess.check_output([str(binary),str(root/name),str(limit),*map(str,parts)],text=True,timeout=20).splitlines()
   return out
  zip=root/"fixture.zip"
  with zipfile.ZipFile(zip,"w",compression=zipfile.ZIP_DEFLATED) as z:z.writestr("nested/game.ffpfsc",DATA)
  assert run("zip",[zip])[0]=="completed"
  assert (root/"zip/game.ffpfsc").read_bytes()==DATA
  tar=root/"fixture.tar"
  with tarfile.open(tar,"w") as t:
   entry=tarfile.TarInfo("nested/file.bin");entry.size=len(DATA);t.addfile(entry,io.BytesIO(DATA))
  assert run("tar",[tar])[0]=="completed"
  # Real archive split byte stream: ordered volumes, no temporary joined copy.
  blob=tar.read_bytes();parts=[]
  for i in range(3):
   p=root/f"tar-part{i}";p.write_bytes(blob[i*len(blob)//3:(i+1)*len(blob)//3]);parts.append(p)
  assert run("split-tar",parts)[0]=="completed"
  assert (root/"split-tar/nested/file.bin").read_bytes()==DATA
  plain=root/"payload.bin";plain.write_bytes(DATA)
  seven=root/"fixture.7z"
  subprocess.run([prefix+"/bin/bsdtar","--format=7zip","-cf",str(seven),"-C",str(root),"payload.bin"],check=True)
  assert run("7zip",[seven])[0]=="completed"
  assert (root/"7zip/payload.bin").read_bytes()==DATA
  blob=seven.read_bytes();seven_parts=[]
  for i in range(3):
   p=root/f"seven-part{i}";p.write_bytes(blob[i*len(blob)//3:(i+1)*len(blob)//3]);seven_parts.append(p)
  result=run("split-7zip",seven_parts)
  assert result[0]=="completed",result
  assert (root/"split-7zip/payload.bin").read_bytes()==DATA
  assert run("missing-part",parts[:1])[0]=="failed"
  assert not (root/"missing-part").exists()
  rar_parts=[]
  for source in sorted((ROOT/"tools/tests/archives").glob("*.rar.uu")):
   lines=source.read_bytes().splitlines();at=next(i for i,line in enumerate(lines) if line.startswith(b"begin "))
   content=b"".join(binascii.a2b_uu(line) for line in lines[at+1:] if line not in (b"end",b"`",b" ",b""))
   dest=root/source.stem;dest.write_bytes(content);rar_parts.append(dest)
  assert len(rar_parts)==3
  result=run("rar-multipart",rar_parts)
  assert result[0]=="completed",result
  assert any(p.is_file() and p.stat().st_size>0 for p in (root/"rar-multipart").rglob("*") if p.name!=".ps5-react-extraction")
  # A process interruption leaves owned staging; the next extraction restarts safely.
  st=zip.stat();identity=f"{zip}|{st.st_dev}|{st.st_ino}|{st.st_size}|{st.st_mtime_ns//10**9}|{st.st_mtime_ns%10**9}\n"
  staging=root/"interrupted.extracting";staging.mkdir();(staging/"unfinished").write_bytes(b"partial")
  (staging/".ps5-react-stage").write_text("P5ST001:"+hashlib.sha256(identity.encode()).hexdigest()+"\n")
  assert run("interrupted",[zip])[0]=="completed"
  assert (root/"interrupted/game.ffpfsc").read_bytes()==DATA
  orphan=root/"orphan.extracting";orphan.mkdir();(orphan/"keep").write_bytes(b"unowned")
  assert run("orphan",[zip])[0]=="failed"
  assert (orphan/"keep").read_bytes()==b"unowned"
  assert run("limit",[zip],100)[0]=="failed"
  assert not (root/"limit").exists()
  for name,path in [("traversal","../escape"),("absolute","/escape"),("drive","C:/escape")]:
   p=root/f"{name}.zip"
   with zipfile.ZipFile(p,"w") as z:z.writestr(path,b"bad")
   assert run(name,[p])[0]=="failed"
   assert not (root/name).exists()
  p=root/"symlink.tar"
  with tarfile.open(p,"w") as t:
   entry=tarfile.TarInfo("link");entry.type=tarfile.SYMTYPE;entry.linkname="../escape";t.addfile(entry)
  assert run("symlink",[p])[0]=="failed"
  assert not (root/"symlink").exists()
  assert run("zip",[zip])[0]=="completed" # Owned output is hash-verified on recovery.
  assert (root/"zip/game.ffpfsc").read_bytes()==DATA
  (root/"zip/game.ffpfsc").write_bytes(b"corrupt")
  assert run("zip",[zip])[0]=="failed"
  print("PASS: ZIP/TAR/7z/multivolume RAR, interrupted staging, verified recovery, missing parts and unsafe archive rejection")
if __name__=="__main__":main()
