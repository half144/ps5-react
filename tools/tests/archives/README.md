# Archive test fixtures

The three `test_rar_multivolume_single_file.part*.rar.uu` fixtures come from
[libarchive v3.7.4](https://github.com/libarchive/libarchive/tree/v3.7.4/libarchive/test).
They exercise a real multivolume RAR without game data. Preserve the upstream
BSD notice in `LIBARCHIVE-LICENSE`. ZIP, TAR and 7z fixtures are generated locally.

The four `rar5_encrypted_volumes.part*.rar.uu` fixtures were made with RAR 7.12
(`rar a -ma5 -m3 -hpfixture-password -v20k`) from the generated text that
`words()` in `tools/test_archives.py` rebuilds. Their compressed blocks and
AES-CBC data cross volumes at unaligned offsets, as in real game uploads.
