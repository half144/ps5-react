"""Real local archive extraction with the same native worker used on PS5."""
from pathlib import Path
import binascii,hashlib,io,json,lzma,struct,subprocess,tarfile,tempfile,zipfile
ROOT=Path(__file__).resolve().parents[1]
DATA=bytes(range(256))*1024
TOO_BIG="This archive needs more memory to extract than the console app can use."

def number7z(v):
 for n in range(8):
  if v<1<<(8*n+7-n):return bytes([(0xFF00>>n)&0xFF|v>>(8*n)])+(v&((1<<8*n)-1)).to_bytes(n,"little")
 return b"\xff"+v.to_bytes(8,"little")

def encode_7z_header(plain):
 """Rewrites a bsdtar 7z with the LZMA-compressed header 7-Zip writes by default."""
 offset,size=struct.unpack("<QQ",plain[12:28]);header=plain[32+offset:32+offset+size]
 packed=lzma.compress(header,format=lzma.FORMAT_RAW,filters=[{"id":lzma.FILTER_LZMA1,"dict_size":1<<16}])
 coder=b"\x23\x03\x01\x01"+number7z(5)+b"\x5d"+(1<<16).to_bytes(4,"little")
 encoded=(b"\x17\x06"+number7z(offset)+number7z(1)+b"\x09"+number7z(len(packed))+b"\x00"
  +b"\x07\x0b"+number7z(1)+b"\x00"+number7z(1)+coder+b"\x0c"+number7z(len(header))+b"\x00\x00")
 start=struct.pack("<QQI",offset+len(packed),len(encoded),binascii.crc32(encoded))
 return plain[:8]+struct.pack("<I",binascii.crc32(start))+start+plain[32:32+offset]+packed+encoded

def vint(v):
 out=b""
 while v>=0x80:out+=bytes([v&0x7f|0x80]);v>>=7
 return out+bytes([v])

def rar5_block(header,data=b""):
 size=bytes([len(header)])
 return struct.pack("<I",binascii.crc32(size+header))+size+header+data

def rar5_with_window(shift):
 """A RAR5 archive whose only file declares a 128 KiB << shift dictionary."""
 data=bytes(64)
 file=bytes([2,2,len(data),0,len(data),0])+vint(3<<7|shift<<10)+bytes([0,1])+b"a"
 return b"Rar!\x1a\x07\x01\x00"+rar5_block(bytes([1,0,0]))+rar5_block(file,data)+rar5_block(bytes([5,0,0]))

def main():
 with tempfile.TemporaryDirectory(prefix="ps5-react-archives-") as tmp:
  root=Path(tmp);binary=root/"archive-client"
  prefix=subprocess.check_output(["brew","--prefix","libarchive"],text=True).strip()
  xz=subprocess.check_output(["brew","--prefix","xz"],text=True).strip()
  sources=[ROOT/"tools/tests/archive_client.cpp",ROOT/"native/shared/archives.cpp",ROOT/"native/shared/archive_preflight.cpp"]
  subprocess.run(["clang++","-std=c++20","-O2","-Wall","-Wextra","-Werror","-pthread","-I",str(ROOT/"native/shared"),"-I",prefix+"/include","-I",xz+"/include",*map(str,sources),"-L",prefix+"/lib","-L",xz+"/lib","-larchive","-llzma","-o",str(binary)],check=True)
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
  # Decoder windows above the console budget are refused before libarchive allocates them.
  big=root/"big.7z"
  subprocess.run([prefix+"/bin/bsdtar","--format=7zip","--options=7zip:compression=lzma2,7zip:compression-level=9","-cf",str(big),"-C",str(root),"payload.bin"],check=True)
  # 7-Zip ultra's 64 MiB dictionary is the budget itself.
  assert run("big-7zip",[big])[0]=="completed"
  encoded=root/"encoded.7z";encoded.write_bytes(encode_7z_header(seven.read_bytes()))
  result=run("encoded-7zip",[encoded])
  assert result[0]=="completed",result
  assert (root/"encoded-7zip/payload.bin").read_bytes()==DATA
  encoded_big=root/"encoded-big.7z";encoded_big.write_bytes(encode_7z_header(big.read_bytes()))
  assert run("encoded-big-7zip",[encoded_big])[0]=="completed"
  p=root/"preset9.tar.xz";p.write_bytes(lzma.compress(tar.read_bytes(),preset=9))
  assert run("xz9",[p])[0]=="completed"
  p=root/"huge.tar.xz"
  p.write_bytes(lzma.compress(tar.read_bytes(),format=lzma.FORMAT_XZ,filters=[{"id":lzma.FILTER_LZMA2,"dict_size":128<<20}]))
  assert run("xz-huge",[p])[1:]==["0",TOO_BIG]
  assert not (root/"xz-huge").exists() and not (root/"xz-huge.extracting").exists()
  p=root/"lzma.zip"
  with zipfile.ZipFile(p,"w",compression=zipfile.ZIP_LZMA) as z:z.writestr("file.bin",DATA)
  assert run("zip-lzma",[p])[0]=="completed"
  p=root/"window.rar";p.write_bytes(rar5_with_window(9))
  assert run("rar5-window",[p])[1:]==["0",TOO_BIG]
  # WinRAR 7's default 32 MB dictionary, which libarchive doubles to the 64 MiB budget.
  p=root/"small-window.rar";p.write_bytes(rar5_with_window(8))
  assert run("rar5-small-window",[p])[2]!=TOO_BIG
  # A sparse member arrives out of order, so its digest comes from reading the file back.
  sparse=root/"sparse.bin"
  with sparse.open("wb") as f:f.write(b"head"*1000);f.seek(8*1024*1024);f.write(b"tail"*1000)
  p=root/"sparse.tar";subprocess.run([prefix+"/bin/bsdtar","--read-sparse","-cf",str(p),"-C",str(root),"sparse.bin"],check=True)
  assert run("sparse",[p],16*1024*1024)[0]=="completed"
  assert (root/"sparse/.ps5-react-extraction").read_text().splitlines()[1]==hashlib.sha256(sparse.read_bytes()).hexdigest()+"|sparse.bin"
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
  print("PASS: ZIP/TAR/7z/multivolume RAR, decoder memory refusal, interrupted staging, verified recovery, missing parts and unsafe archive rejection")
if __name__=="__main__":main()
