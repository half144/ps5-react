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

def encrypted_rar5(data,password="DLPSGAME.COM",name="nested/game.ffpfsc",headers=True,volumes=1,compression=0,crypto_flags=1):
 """Independent RAR5 fixture with AES-CBC data/headers and spec password checks."""
 salt=bytes(range(16));power=15;iterations=1<<power
 key=hashlib.pbkdf2_hmac("sha256",password.encode(),salt,iterations)
 check=hashlib.pbkdf2_hmac("sha256",password.encode(),salt,iterations+32)
 folded=bytes(check[i]^check[i+8]^check[i+16]^check[i+24] for i in range(8))
 password_check=folded+hashlib.sha256(folded).digest()[:4]
 def aes(plain,iv):
  plain+=bytes((-len(plain))%16)
  return subprocess.check_output(["openssl","enc","-aes-256-cbc","-nopad","-K",key.hex(),"-iv",iv.hex()],input=plain)
 def block(body):
  size=vint(len(body));return struct.pack("<I",binascii.crc32(size+body))+size+body
 def framed(body):
  plain=block(body);iv=bytes(range(16,32))
  return iv+aes(plain,iv) if headers else plain
 out=[]
 for i in range(volumes):
  iv=bytes(range(32,48));piece=data[i*len(data)//volumes:(i+1)*len(data)//volumes]
  packed=aes(piece or bytes(16),iv)
  encryption=vint(1)+vint(0)+vint(crypto_flags)+bytes([power])+salt+iv+password_check
  extra=vint(len(encryption))+encryption
  common=3|(8 if i else 0)|(16 if i+1<volumes else 0)
  body=vint(2)+vint(common)+vint(len(extra))+vint(len(packed))+vint(4)+vint(len(data))+vint(0)
  body+=struct.pack("<I",binascii.crc32(data))+vint(compression)+vint(0)+vint(len(name.encode()))+name.encode()+extra
  prefix=b"Rar!\x1a\x07\x01\x00"
  if headers:prefix+=block(vint(4)+vint(0)+vint(0)+vint(1)+bytes([power])+salt+password_check)
  main=vint(1)+vint(0)+(vint(3)+vint(i) if volumes>1 else vint(0))
  out.append(prefix+framed(main)+framed(body)+packed+framed(vint(5)+vint(0)+vint(int(i+1<volumes))))
 return out

def main():
 with tempfile.TemporaryDirectory(prefix="ps5-react-archives-") as tmp:
  root=Path(tmp);binary=root/"archive-client"
  prefix=subprocess.check_output(["brew","--prefix","libarchive"],text=True).strip()
  xz=subprocess.check_output(["brew","--prefix","xz"],text=True).strip()
  crypto=subprocess.check_output(["brew","--prefix","openssl@3"],text=True).strip()
  sources=[ROOT/"tools/tests/archive_client.cpp",ROOT/"native/shared/archives.cpp",ROOT/"native/shared/archive_preflight.cpp",ROOT/"native/shared/rar5_password.cpp"]
  subprocess.run(["clang++","-std=c++20","-O2","-Wall","-Wextra","-Werror","-pthread","-I",str(ROOT/"native/shared"),"-I",prefix+"/include","-I",xz+"/include","-I",crypto+"/include",*map(str,sources),"-L",prefix+"/lib","-L",xz+"/lib","-L",crypto+"/lib","-larchive","-llzma","-lcrypto","-lz","-o",str(binary)],check=True)
  def run(name,parts,limit=1024*1024,password=None):
   args=[] if password is None else ["--password",password]
   out=subprocess.check_output([str(binary),str(root/name),str(limit),*map(str,parts),*args],text=True,timeout=20).splitlines()
   return out
  zip=root/"fixture.zip"
  with zipfile.ZipFile(zip,"w",compression=zipfile.ZIP_DEFLATED) as z:z.writestr("nested/game.ffpfsc",DATA)
  assert run("zip",[zip])[0]=="completed"
  assert (root/"zip/game.ffpfsc").read_bytes()==DATA
  # Real password-protected ZIP: correct password, incorrect/missing password, and ordered volumes.
  protected=root/"protected.zip";payload=root/"protected.bin";payload.write_bytes(DATA)
  subprocess.run(["/usr/bin/zip","-q","-P","DLPSGAME.COM",str(protected),payload.name],cwd=root,check=True)
  assert run("zip-password",[protected],password="DLPSGAME.COM")[0]=="completed"
  assert (root/"zip-password/protected.bin").read_bytes()==DATA
  for name,password in [("wrong-password","wrong"),("missing-password",None)]:
   assert run(name,[protected],password=password)[0]=="failed"
   assert protected.exists() and not (root/name).exists() and not (root/(name+".extracting")).exists()
  blob=protected.read_bytes();protected_parts=[]
  for i in range(3):
   volume=root/f"protected-part{i}";volume.write_bytes(blob[i*len(blob)//3:(i+1)*len(blob)//3]);protected_parts.append(volume)
  assert run("zip-password-volumes",protected_parts,password="DLPSGAME.COM")[0]=="completed"
  assert (root/"zip-password-volumes/protected.bin").read_bytes()==DATA
  for headers in [True,False]:
   name="rar5-encrypted-headers" if headers else "rar5-encrypted-data"
   rar=root/(name+".rar");rar.write_bytes(encrypted_rar5(DATA,headers=headers)[0])
   result=run(name,[rar],password="DLPSGAME.COM")
   assert result[0]=="completed",result
   assert (root/name/"game.ffpfsc").read_bytes()==DATA
   result=run(name+"-wrong",[rar],password="wrong")
   assert result[0]=="failed" and "password" in result[-1].lower(),result
   assert rar.exists() and not (root/(name+"-wrong")).exists()
  rar_parts=[]
  for i,blob in enumerate(encrypted_rar5(DATA,volumes=2)):
   volume=root/f"encrypted-volume-{i}.rar";volume.write_bytes(blob);rar_parts.append(volume)
  result=run("rar5-password-volumes",rar_parts,password="DLPSGAME.COM")
  assert result[0]=="completed",result
  assert (root/"rar5-password-volumes/game.ffpfsc").read_bytes()==DATA
  corrupt=root/"corrupt-encrypted.rar"
  damaged=bytearray(encrypted_rar5(DATA)[0]);damaged[-64]^=1
  corrupt.write_bytes(damaged)
  result=run("rar5-corrupt",[corrupt],password="DLPSGAME.COM")
  assert result[0]=="failed" and "checksum" in result[-1].lower(),result
  assert corrupt.exists() and not (root/"rar5-corrupt").exists()
  empty=root/"empty-encrypted.rar";empty.write_bytes(encrypted_rar5(b"",compression=4<<7|8<<10)[0])
  result=run("rar5-empty",[empty],password="DLPSGAME.COM");assert result[0]=="completed",result
  assert (root/"rar5-empty/game.ffpfsc").read_bytes()==b""
  for name,kwargs in [("rar5-encrypted-window",{"compression":3<<7|9<<10}),
                       ("rar5-encrypted-traversal",{"name":"../escape"}),
                       ("rar5-keyed-checksum",{"crypto_flags":3})]:
   rar=root/(name+".rar");rar.write_bytes(encrypted_rar5(DATA,**kwargs)[0])
   result=run(name,[rar],password="DLPSGAME.COM")
   assert result[0]=="failed",result
   assert not (root/name).exists() and rar.exists()
  # A non-ASCII name in a process left in the C locale, as on the console (the Teardown backport).
  named=root/"named.zip"
  with zipfile.ZipFile(named,"w") as z:z.writestr("PPSA15246 \u2013 USA/eboot.bin",DATA)
  out=subprocess.check_output([str(binary),str(root/"named"),str(1024*1024),str(named)],text=True,timeout=20,env={"LC_ALL":"C"}).splitlines()
  assert out[0]=="completed",out
  assert (root/"named/PPSA15246 \u2013 USA/eboot.bin").read_bytes()==DATA
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
  # A crash before the ownership record leaves empty staging, which the next extraction takes over.
  (root/"abandoned.extracting").mkdir()
  assert run("abandoned",[zip])[0]=="completed"
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
  # Inspection names a file by its bytes and refuses a too-large window from a download's first bytes.
  def inspect(*paths):
   return subprocess.check_output([str(binary),"inspect",*map(str,paths)],text=True,timeout=20).split("\n")[:2]
  def partial(name,content,keep,hole=0):
   p=root/name;p.write_bytes(content[:keep]+bytes(hole));return p
  big_rar=rar5_with_window(9)
  assert inspect(partial("big.rar.part",big_rar,len(big_rar)-40,1<<20))==["rar",TOO_BIG]
  small_rar=rar5_with_window(8)
  assert inspect(partial("small.rar.part",small_rar,len(small_rar)-40,1<<20))==["rar",""]
  assert inspect(rar_parts[0])==["rar",""]
  assert inspect(partial("hole.part",big_rar,0,1<<20))==["",""]
  assert inspect(partial("zip.part",zip.read_bytes(),200))==["zip",""]
  assert inspect(partial("7z.part",seven.read_bytes(),40))==["7z",""]
  assert inspect(tar)==["tar",""]
  gz=root/"fixture.tar.gz";gz.write_bytes(__import__("gzip").compress(tar.read_bytes()))
  assert inspect(gz)==["tar.gz",""]
  assert inspect(partial("huge.tar.xz.part",(root/"huge.tar.xz").read_bytes(),4096))==["tar.xz",TOO_BIG]
  assert inspect(partial("game.pkg.part",b"\x7fCNT"+bytes(64),68))==["pkg",""]
  assert inspect(partial("game.exfat",b"\xeb\x76\x90EXFAT   "+bytes(500),512))==["exfat",""]
  ufs=bytearray(70000);ufs[65536+1372:65536+1376]=struct.pack("<I",0x19540119)
  assert inspect(partial("game.ffpkg",bytes(ufs),len(ufs)))==["ffpkg",""]
  assert inspect(partial("game.ffpfs",struct.pack("<QQ",1,0x20130315)+bytes(64),80))==["ffpfs",""]
  assert inspect(partial("game.ffpfsc",b"PFSC"+bytes(64),68))==["ffpfsc",""]
  print("PASS: header inspection of partial downloads; ZIP/TAR/7z/multivolume RAR, decoder memory refusal, interrupted staging, verified recovery, missing parts and unsafe archive rejection")
if __name__=="__main__":main()
