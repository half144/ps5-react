// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// RAR5 framing/crypto follows https://www.rarlab.com/technote.htm; no UnRAR code is used.
#include "rar5_password.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace archives {
namespace {
using Bytes = std::vector<unsigned char>;
constexpr std::array<unsigned char, 8> signature{'R', 'a', 'r', '!', 0x1a, 7, 1, 0};
constexpr std::size_t max_header = 2 * 1024 * 1024, block_size = 128 * 1024;
struct Cursor {
  const Bytes &bytes;
  std::size_t at = 0;
  bool ok = true;
  std::uint64_t number() {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      if (at >= bytes.size()) {
        ok = false;
        return 0;
      }
      const auto byte = bytes[at++];
      if (shift == 63 && (byte & 0xfe)) {
        ok = false;
        return 0;
      }
      value |= std::uint64_t(byte & 0x7f) << shift;
      if (!(byte & 0x80))
        return value;
    }
    ok = false;
    return 0;
  }
  bool skip(std::size_t count) {
    if (count > bytes.size() - std::min(at, bytes.size())) {
      ok = false;
      return false;
    }
    at += count;
    return true;
  }
};
void number(Bytes &bytes, std::uint64_t value) {
  do {
    bytes.push_back((value & 127) | (value > 127 ? 128 : 0));
    value >>= 7;
  } while (value);
}
std::uint32_t little32(const unsigned char *p) {
  return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 |
         std::uint32_t(p[3]) << 24;
}
Bytes framed(const Bytes &body) {
  Bytes out;
  number(out, body.size());
  out.insert(out.end(), body.begin(), body.end());
  const auto crc = crc32(0, out.data(), out.size());
  Bytes result;
  for (unsigned i = 0; i < 4; i++)
    result.push_back(crc >> (8 * i));
  result.insert(result.end(), out.begin(), out.end());
  return result;
}
using Cipher = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
Cipher cipher(const std::array<unsigned char, 32> &key, const unsigned char *iv) {
  Cipher ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, key.data(), iv) != 1 ||
      EVP_CIPHER_CTX_set_padding(ctx.get(), 0) != 1)
    ctx.reset();
  return ctx;
}
bool decrypt(Cipher &ctx, const Bytes &encrypted, Bytes &plain) {
  plain.resize(encrypted.size());
  int size = 0;
  return ctx &&
         EVP_DecryptUpdate(ctx.get(), plain.data(), &size, encrypted.data(), encrypted.size()) ==
             1 &&
         std::size_t(size) == encrypted.size();
}
} // namespace

struct Rar5PasswordReader::State {
  const std::vector<std::string> &paths;
  const std::string &password;
  const std::atomic<bool> &cancelled;
  std::size_t volume = 0;
  int fd = -1;
  bool need_signature = true, encrypted_headers = false, finished = false;
  std::array<unsigned char, 32> header_key{};
  Cipher data_cipher{nullptr, EVP_CIPHER_CTX_free};
  std::uint64_t data_left = 0, emit_left = 0, stored_written = 0;
  std::string stored_name, error;
  std::string compressed_name;
  std::uint64_t compressed_left = 0;
  bool compressed_last = false;
  Bytes output;
  std::map<std::string, std::uint32_t> checksums;
  std::size_t checksum_bytes = 0;
  State(const std::vector<std::string> &p, const std::string &pass, const std::atomic<bool> &cancel)
      : paths(p), password(pass), cancelled(cancel) {}
  ~State() {
    if (fd >= 0)
      close(fd);
    OPENSSL_cleanse(header_key.data(), header_key.size());
  }
  bool take(Bytes &bytes, std::size_t size) {
    bytes.resize(size);
    std::size_t at = 0;
    while (at < size && !cancelled) {
      if (fd < 0) {
        if (volume >= paths.size()) {
          error = "Incomplete RAR5 archive or missing volume.";
          return false;
        }
        fd = open(paths[volume++].c_str(), O_RDONLY | O_NOFOLLOW);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
          error = "Cannot read RAR5 source volume.";
          return false;
        }
      }
      const auto count = ::read(fd, bytes.data() + at, size - at);
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0) {
        error = "Cannot read RAR5 source volume.";
        return false;
      }
      if (!count) {
        close(fd);
        fd = -1;
        continue;
      }
      at += count;
    }
    if (cancelled) {
      error = "Extraction cancelled; downloaded parts are preserved.";
      return false;
    }
    return true;
  }
  bool key(Cursor &fields, std::array<unsigned char, 32> &result, bool file) {
    const auto version = fields.number(), flags = fields.number();
    if (!fields.ok || version != 0 || (flags & ~(file ? 3ULL : 1ULL))) {
      error = "Unsupported RAR5 encryption version or flags.";
      return false;
    }
    // Keyed CRC/BLAKE checksums cannot be silently removed: refuse until they can be verified.
    if (file && (flags & 2)) {
      error = "RAR5 keyed checksums are not supported; downloaded parts are preserved.";
      return false;
    }
    if (!fields.skip(17)) {
      error = "Incomplete RAR5 encryption metadata.";
      return false;
    }
    const auto power = fields.bytes[fields.at - 17];
    const auto *salt = fields.bytes.data() + fields.at - 16;
    if (power > 20) {
      error = "RAR5 password derivation exceeds the CPU budget.";
      return false;
    }
    const int iterations = 1 << power;
    if (PKCS5_PBKDF2_HMAC(password.data(), password.size(), salt, 16, iterations, EVP_sha256(),
                          result.size(), result.data()) != 1) {
      error = "Cannot derive RAR5 password key.";
      return false;
    }
    if (file && !fields.skip(16)) {
      error = "Incomplete RAR5 data IV.";
      return false;
    }
    if (flags & 1) {
      if (!fields.skip(12)) {
        error = "Incomplete RAR5 password check.";
        return false;
      }
      const auto *expected = fields.bytes.data() + fields.at - 12;
      std::array<unsigned char, 32> check{}, digest{};
      std::array<unsigned char, 8> folded{};
      SHA256(expected, 8, digest.data());
      if (CRYPTO_memcmp(digest.data(), expected + 8, 4)) {
        error = "Corrupt RAR5 password check.";
        return false;
      }
      if (PKCS5_PBKDF2_HMAC(password.data(), password.size(), salt, 16, iterations + 32,
                            EVP_sha256(), check.size(), check.data()) != 1) {
        error = "Cannot verify RAR5 password.";
        return false;
      }
      for (std::size_t i = 0; i < check.size(); i++)
        folded[i % 8] ^= check[i];
      OPENSSL_cleanse(check.data(), check.size());
      if (CRYPTO_memcmp(folded.data(), expected, 8)) {
        error = "Incorrect RAR5 archive password.";
        return false;
      }
    }
    return true;
  }
  // Read ahead without advancing the stream. Only tiny compressed-block headers are decrypted here;
  // file bodies stay in the 128 KiB streaming buffer. This finds CBC padding before publishing the
  // packed size to libarchive, which would otherwise parse padding as another compression block.
  bool probe(std::uint64_t offset, std::size_t count, Bytes &bytes) {
    if (fd < 0 || volume == 0) {
      error = "Missing RAR5 data source.";
      return false;
    }
    const auto current = lseek(fd, 0, SEEK_CUR);
    if (current < 0) {
      error = "Cannot inspect RAR5 source.";
      return false;
    }
    offset += current;
    bytes.resize(count);
    std::size_t copied = 0;
    for (std::size_t index = volume - 1; index < paths.size() && copied < count; index++) {
      const int input =
          index == volume - 1 ? fd : open(paths[index].c_str(), O_RDONLY | O_NOFOLLOW);
      struct stat st;
      const bool valid = input >= 0 && !fstat(input, &st) && S_ISREG(st.st_mode) && st.st_size >= 0;
      if (!valid) {
        if (input >= 0 && input != fd)
          close(input);
        error = "Cannot inspect RAR5 source.";
        return false;
      }
      if (offset >= static_cast<std::uint64_t>(st.st_size)) {
        offset -= st.st_size;
        if (input != fd)
          close(input);
        continue;
      }
      const auto available = std::min<std::uint64_t>(count - copied, st.st_size - offset);
      std::size_t got = 0;
      while (got < available) {
        const auto n = pread(input, bytes.data() + copied + got, available - got, offset + got);
        if (n < 0 && errno == EINTR)
          continue;
        if (n <= 0)
          break;
        got += n;
      }
      if (input != fd)
        close(input);
      if (got != available) {
        error = "Incomplete RAR5 compressed data.";
        return false;
      }
      copied += got;
      offset = 0;
    }
    if (copied != count) {
      error = "Incomplete RAR5 compressed data.";
      return false;
    }
    return true;
  }
  bool compressed_size(std::uint64_t packed, std::uint64_t unpacked, std::uint64_t flags,
                       const std::string &name, const std::array<unsigned char, 32> &data_key,
                       const unsigned char *data_iv, std::uint64_t &emitted) {
    if (!(flags & 8) || compressed_name != name) {
      compressed_name = name;
      compressed_left = 0;
      compressed_last = false;
    }
    if (!unpacked) {
      emitted = 0;
      return true;
    }
    std::uint64_t at = std::min(compressed_left, packed);
    compressed_left -= at;
    if (compressed_left) {
      if (!(flags & 16)) {
        error = "Incomplete RAR5 compressed volume.";
        return false;
      }
      emitted = packed;
      return true;
    }
    for (unsigned blocks = 0; blocks < 100000 && !cancelled; blocks++) {
      if (compressed_last) {
        if (packed - at > 15) {
          error = "Invalid RAR5 encrypted padding.";
          return false;
        }
        emitted = at;
        return true;
      }
      if (at == packed) {
        if (flags & 16) {
          emitted = packed;
          return true;
        }
        break;
      }
      const auto aligned = at & ~std::uint64_t(15), within = at - aligned;
      const auto count =
          std::min<std::uint64_t>((within + 5 + 15) & ~std::uint64_t(15), packed - aligned);
      Bytes iv(data_iv, data_iv + 16), raw, plain;
      if ((aligned && !probe(aligned - 16, 16, iv)) || !probe(aligned, count, raw))
        return false;
      auto ctx = cipher(data_key, iv.data());
      if (!decrypt(ctx, raw, plain)) {
        error = "Cannot inspect RAR5 compressed block.";
        return false;
      }
      if (within + 3 > plain.size()) {
        error = "Incomplete RAR5 compressed block header.";
        return false;
      }
      const auto block_flags = plain[within];
      const unsigned size_bytes = ((unsigned(block_flags) >> 3) & 7) + 1;
      if (size_bytes > 3 || within + 2 + size_bytes > plain.size()) {
        error = "Unsupported RAR5 compressed block header.";
        return false;
      }
      std::uint64_t size = 0;
      unsigned char checksum = 0x5a ^ block_flags;
      for (unsigned i = 0; i < size_bytes; i++) {
        size |= std::uint64_t(plain[within + 2 + i]) << (8 * i);
        checksum ^= plain[within + 2 + i];
      }
      if (checksum != plain[within + 1]) {
        error = "Corrupt RAR5 compressed block header.";
        return false;
      }
      at += 2 + size_bytes;
      compressed_last = block_flags & 0x40;
      if (size > packed - at) {
        compressed_left = size - (packed - at);
        if (!(flags & 16)) {
          error = "Incomplete RAR5 compressed volume.";
          return false;
        }
        emitted = packed;
        return true;
      }
      at += size;
    }
    error = "Missing RAR5 last compression block or compression-block budget exceeded.";
    return false;
  }
  bool header(Bytes &plain) {
    Cipher ctx(nullptr, EVP_CIPHER_CTX_free);
    Bytes first;
    if (encrypted_headers) {
      Bytes iv;
      if (!take(iv, 16) || !take(first, 16))
        return false;
      ctx = cipher(header_key, iv.data());
      if (!decrypt(ctx, first, plain)) {
        error = "Cannot decrypt RAR5 header.";
        return false;
      }
    } else {
      if (!take(plain, 5))
        return false;
      for (unsigned extra = 0; plain.back() & 128; extra++) {
        if (extra == 2) {
          error = "Invalid RAR5 header size.";
          return false;
        }
        Bytes byte;
        if (!take(byte, 1))
          return false;
        plain.push_back(byte[0]);
      }
    }
    Cursor length{plain, 4};
    const auto size = length.number();
    const auto total = length.at + size;
    if (!length.ok || !size || size > max_header || length.at > 7) {
      error = "Invalid RAR5 header size.";
      return false;
    }
    const auto required = encrypted_headers ? (total + 15) & ~std::size_t(15) : total;
    if (required > plain.size()) {
      Bytes tail;
      if (!take(tail, required - plain.size()))
        return false;
      if (encrypted_headers) {
        Bytes decoded;
        if (!decrypt(ctx, tail, decoded)) {
          error = "Cannot decrypt RAR5 header.";
          return false;
        }
        plain.insert(plain.end(), decoded.begin(), decoded.end());
      } else
        plain.insert(plain.end(), tail.begin(), tail.end());
    }
    plain.resize(total);
    if (little32(plain.data()) != crc32(0, plain.data() + 4, plain.size() - 4)) {
      error = encrypted_headers ? "Incorrect RAR5 password or corrupt encrypted header."
                                : "Corrupt RAR5 header.";
      return false;
    }
    return true;
  }
  bool prepare(const Bytes &plain) {
    Cursor fields{plain, 4};
    fields.number();
    const auto type = fields.number(), flags = fields.number();
    const auto extra_size = flags & 1 ? fields.number() : 0;
    const auto packed = flags & 2 ? fields.number() : 0;
    const auto common_end = fields.at;
    if (!fields.ok || extra_size > plain.size() - std::min(fields.at, plain.size())) {
      error = "Invalid RAR5 header fields.";
      return false;
    }
    const auto extra_at = plain.size() - extra_size;
    if (type == 4) {
      if (encrypted_headers || packed) {
        error = "Invalid RAR5 archive encryption header.";
        return false;
      }
      if (!key(fields, header_key, false))
        return false;
      encrypted_headers = true;
      output.clear();
      return true;
    }
    std::uint64_t emitted = packed;
    data_cipher.reset();
    Bytes kept_extra, specific;
    if (type == 2 || type == 3) {
      const auto file_flags = fields.number(), unpacked = fields.number();
      fields.number();
      if (file_flags & 2)
        fields.skip(4);
      const auto crc_at = fields.at;
      if (file_flags & 4)
        fields.skip(4);
      const auto compression_at = fields.at;
      const auto compression = fields.number();
      const auto compression_end = fields.at;
      fields.number();
      const auto name_size = fields.number();
      const auto name_at = fields.at;
      fields.skip(name_size);
      if (!fields.ok || fields.at > extra_at) {
        error = "Invalid RAR5 file header.";
        return false;
      }
      const auto method = (compression >> 7) & 7;
      const auto window = 2 * (128ULL * 1024 << ((compression >> 10) & 31));
      if (!(file_flags & 1) && method && window > 64 * 1024 * 1024) {
        error = "This archive needs more memory to extract than the console app can use.";
        return false;
      }
      if (file_flags & 8) {
        error = "RAR5 files with unknown unpacked size are not supported.";
        return false;
      }
      const std::string name(reinterpret_cast<const char *>(plain.data() + name_at), name_size);
      if (type == 3 && name == "QO") {
        // Quick-open is an optional index of cached headers. Read the original headers instead;
        // its service encryption check need not authenticate the password used for file data.
        // The enclosing header CRC was already checked, and take() still validates payload length.
        data_left = packed;
        emit_left = 0;
        output.clear();
        return true;
      }
      if (type == 2 && !(file_flags & 1) && (file_flags & 4)) {
        if (!checksums.contains(name)) {
          checksum_bytes += name.size() + 64;
          if (checksum_bytes > 16 * 1024 * 1024 || checksums.size() >= 100000) {
            error = "RAR5 checksum metadata exceeds the limit.";
            return false;
          }
        }
        checksums[name] = little32(plain.data() + crc_at);
      }
      Cursor extra{plain, extra_at};
      bool has_crypto = false;
      while (extra.at < plain.size()) {
        const auto start = extra.at;
        const auto size = extra.number();
        if (!extra.ok || size > plain.size() - std::min(extra.at, plain.size())) {
          error = "Invalid RAR5 extra record.";
          return false;
        }
        const auto end = extra.at + size, record = extra.number();
        if (!extra.ok || extra.at > end) {
          error = "Invalid RAR5 extra record.";
          return false;
        }
        if (record == 1) {
          if (has_crypto) {
            error = "Duplicate RAR5 encryption record.";
            return false;
          }
          has_crypto = true;
          if (type == 2 && !(file_flags & 1) && !(file_flags & 4)) {
            error = "Encrypted RAR5 files without CRC32 are not supported.";
            return false;
          }
          Bytes record_bytes(plain.begin() + extra.at, plain.begin() + end);
          Cursor encryption{record_bytes};
          std::array<unsigned char, 32> data_key{};
          if (!key(encryption, data_key, true))
            return false;
          Cursor iv_fields{record_bytes};
          iv_fields.number();
          iv_fields.number();
          iv_fields.skip(17);
          const auto *iv = record_bytes.data() + iv_fields.at;
          data_cipher = cipher(data_key, iv);
          if (!data_cipher || packed % 16) {
            error = "Invalid RAR5 encrypted data alignment.";
            return false;
          }
          if (!method) {
            if (!(flags & 8) || stored_name != name) {
              stored_name = name;
              stored_written = 0;
            }
            if (stored_written > unpacked) {
              error = "Invalid RAR5 stored volume size.";
              return false;
            }
            emitted = std::min(packed, unpacked - stored_written);
            stored_written += emitted;
          } else if (!compressed_size(packed, unpacked, flags, name, data_key, iv, emitted)) {
            OPENSSL_cleanse(data_key.data(), data_key.size());
            return false;
          }
          if (!unpacked && method) {
            // RAR stores an encrypted padding block for empty compressed files. Once removed,
            // mark the empty file as stored so libarchive does not read the next header as data.
            specific.insert(specific.end(), plain.begin() + common_end,
                            plain.begin() + compression_at);
            number(specific, compression & ~(7ULL << 7));
            specific.insert(specific.end(), plain.begin() + compression_end,
                            plain.begin() + extra_at);
          }
          OPENSSL_cleanse(data_key.data(), data_key.size());
        } else
          kept_extra.insert(kept_extra.end(), plain.begin() + start, plain.begin() + end);
        extra.at = end;
      }
    } else
      kept_extra.insert(kept_extra.end(), plain.begin() + extra_at, plain.end());
    data_left = packed;
    emit_left = emitted;
    Bytes body;
    number(body, type);
    number(body, kept_extra.empty() ? flags & ~1ULL : flags | 1ULL);
    if (!kept_extra.empty())
      number(body, kept_extra.size());
    if (flags & 2)
      number(body, emitted);
    if (specific.empty())
      body.insert(body.end(), plain.begin() + common_end, plain.begin() + extra_at);
    else
      body.insert(body.end(), specific.begin(), specific.end());
    body.insert(body.end(), kept_extra.begin(), kept_extra.end());
    output = framed(body);
    if (type == 5) {
      const auto more = fields.number();
      if (!fields.ok) {
        error = "Invalid RAR5 end header.";
        return false;
      }
      if (more & 1) {
        need_signature = true;
        encrypted_headers = false;
      } else
        finished = true;
    }
    return true;
  }
  la_ssize_t next() {
    output.clear();
    while (error.empty() && !cancelled) {
      if (data_left) {
        const auto count = std::min<std::uint64_t>(data_left, block_size);
        Bytes raw;
        if (!take(raw, count))
          return -1;
        if (data_cipher) {
          if (!decrypt(data_cipher, raw, output)) {
            error = "Cannot decrypt RAR5 data.";
            return -1;
          }
        } else
          output = std::move(raw);
        data_left -= count;
        const auto emit = std::min<std::uint64_t>(emit_left, count);
        emit_left -= emit;
        output.resize(emit);
        if (!data_left)
          data_cipher.reset();
        if (emit)
          return output.size();
        continue;
      }
      if (finished)
        return 0;
      if (need_signature) {
        if (!take(output, signature.size()))
          return -1;
        if (!std::equal(signature.begin(), signature.end(), output.begin())) {
          error = "Missing RAR5 volume signature.";
          return -1;
        }
        need_signature = false;
        return output.size();
      }
      Bytes plain;
      if (!header(plain) || !prepare(plain))
        return -1;
      if (!output.empty())
        return output.size();
    }
    if (cancelled)
      error = "Extraction cancelled; downloaded parts are preserved.";
    return -1;
  }
};
bool Rar5PasswordReader::matches(const std::string &path) {
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0)
    return false;
  std::array<unsigned char, 8> bytes{};
  const auto count = ::read(fd, bytes.data(), bytes.size());
  close(fd);
  return count == 8 && bytes == signature;
}
Rar5PasswordReader::Rar5PasswordReader(const std::vector<std::string> &paths,
                                       const std::string &password,
                                       const std::atomic<bool> &cancelled)
    : state(std::make_unique<State>(paths, password, cancelled)) {}
Rar5PasswordReader::~Rar5PasswordReader() = default;
const std::string &Rar5PasswordReader::error() const {
  return state->error;
}
la_ssize_t Rar5PasswordReader::read(archive *archive, void *client, const void **buffer) {
  auto &state = *static_cast<Rar5PasswordReader *>(client)->state;
  const auto count = state.next();
  *buffer = state.output.data();
  if (count < 0)
    archive_set_error(archive, EINVAL, "%s", state.error.c_str());
  return count;
}
bool Rar5PasswordReader::verify(const std::string &name, std::uint32_t crc) const {
  const auto found = state->checksums.find(name);
  return found == state->checksums.end() || found->second == crc;
}
} // namespace archives
