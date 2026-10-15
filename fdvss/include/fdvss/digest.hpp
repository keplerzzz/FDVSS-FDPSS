#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif

namespace fdvss {

using Digest256 = std::array<std::uint8_t, 32>;

#if defined(__APPLE__)

class Sha256 {
  public:
    Sha256() { CC_SHA256_Init(&context_); }

    void update(const void* data, std::size_t size) {
        CC_SHA256_Update(&context_, data, static_cast<CC_LONG>(size));
    }

    Digest256 finish() {
        Digest256 digest{};
        CC_SHA256_Final(digest.data(), &context_);
        return digest;
    }

  private:
    CC_SHA256_CTX context_{};
};

#else

// Portable SHA-256 fallback. It is used only when a platform crypto primitive
// is unavailable, keeping the experiment artifact dependency-free.
class Sha256 {
  public:
    Sha256() = default;

    void update(const void* raw, std::size_t size) {
        const auto* input = static_cast<const std::uint8_t*>(raw);
        total_bytes_ += size;
        while (size != 0) {
            const std::size_t take = (size < 64 - buffered_) ? size : 64 - buffered_;
            std::memcpy(buffer_.data() + buffered_, input, take);
            buffered_ += take;
            input += take;
            size -= take;
            if (buffered_ == 64) {
                transform(buffer_.data());
                buffered_ = 0;
            }
        }
    }

    Digest256 finish() {
        const std::uint64_t bit_length = static_cast<std::uint64_t>(total_bytes_) * 8U;
        buffer_[buffered_++] = 0x80;
        if (buffered_ > 56) {
            while (buffered_ < 64) buffer_[buffered_++] = 0;
            transform(buffer_.data());
            buffered_ = 0;
        }
        while (buffered_ < 56) buffer_[buffered_++] = 0;
        for (int i = 7; i >= 0; --i) {
            buffer_[buffered_++] = static_cast<std::uint8_t>(bit_length >> (i * 8));
        }
        transform(buffer_.data());

        Digest256 digest{};
        for (std::size_t i = 0; i < state_.size(); ++i) {
            digest[4 * i] = static_cast<std::uint8_t>(state_[i] >> 24);
            digest[4 * i + 1] = static_cast<std::uint8_t>(state_[i] >> 16);
            digest[4 * i + 2] = static_cast<std::uint8_t>(state_[i] >> 8);
            digest[4 * i + 3] = static_cast<std::uint8_t>(state_[i]);
        }
        return digest;
    }

  private:
    static constexpr std::array<std::uint32_t, 64> constants_ = {
        0x428a2f98U,0x71374491U,0xb5c0fbcfU,0xe9b5dba5U,0x3956c25bU,0x59f111f1U,0x923f82a4U,0xab1c5ed5U,
        0xd807aa98U,0x12835b01U,0x243185beU,0x550c7dc3U,0x72be5d74U,0x80deb1feU,0x9bdc06a7U,0xc19bf174U,
        0xe49b69c1U,0xefbe4786U,0x0fc19dc6U,0x240ca1ccU,0x2de92c6fU,0x4a7484aaU,0x5cb0a9dcU,0x76f988daU,
        0x983e5152U,0xa831c66dU,0xb00327c8U,0xbf597fc7U,0xc6e00bf3U,0xd5a79147U,0x06ca6351U,0x14292967U,
        0x27b70a85U,0x2e1b2138U,0x4d2c6dfcU,0x53380d13U,0x650a7354U,0x766a0abbU,0x81c2c92eU,0x92722c85U,
        0xa2bfe8a1U,0xa81a664bU,0xc24b8b70U,0xc76c51a3U,0xd192e819U,0xd6990624U,0xf40e3585U,0x106aa070U,
        0x19a4c116U,0x1e376c08U,0x2748774cU,0x34b0bcb5U,0x391c0cb3U,0x4ed8aa4aU,0x5b9cca4fU,0x682e6ff3U,
        0x748f82eeU,0x78a5636fU,0x84c87814U,0x8cc70208U,0x90befffaU,0xa4506cebU,0xbef9a3f7U,0xc67178f2U
    };

    static constexpr std::uint32_t rotate(std::uint32_t x, unsigned n) {
        return (x >> n) | (x << (32U - n));
    }

    void transform(const std::uint8_t* block) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            words[i] = (static_cast<std::uint32_t>(block[4*i]) << 24) |
                       (static_cast<std::uint32_t>(block[4*i+1]) << 16) |
                       (static_cast<std::uint32_t>(block[4*i+2]) << 8) |
                       static_cast<std::uint32_t>(block[4*i+3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotate(words[i-15], 7) ^ rotate(words[i-15], 18) ^ (words[i-15] >> 3);
            const std::uint32_t s1 = rotate(words[i-2], 17) ^ rotate(words[i-2], 19) ^ (words[i-2] >> 10);
            words[i] = words[i-16] + s0 + words[i-7] + s1;
        }

        auto [a,b,c,d,e,f,g,h] = state_;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
            const std::uint32_t choose = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + choose + constants_[i] + words[i];
            const std::uint32_t s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;
            h=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
        }
        state_[0]+=a; state_[1]+=b; state_[2]+=c; state_[3]+=d;
        state_[4]+=e; state_[5]+=f; state_[6]+=g; state_[7]+=h;
    }

    std::array<std::uint32_t, 8> state_ = {
        0x6a09e667U,0xbb67ae85U,0x3c6ef372U,0xa54ff53aU,
        0x510e527fU,0x9b05688cU,0x1f83d9abU,0x5be0cd19U
    };
    std::array<std::uint8_t, 64> buffer_{};
    std::size_t buffered_ = 0;
    std::size_t total_bytes_ = 0;
};

#endif

inline void sha_update_u64(Sha256& hash, std::uint64_t value) {
    std::array<std::uint8_t, 8> encoded{};
    for (std::size_t i = 0; i < 8; ++i) {
        encoded[i] = static_cast<std::uint8_t>(value >> (8U * i));
    }
    hash.update(encoded.data(), encoded.size());
}

}  // namespace fdvss

