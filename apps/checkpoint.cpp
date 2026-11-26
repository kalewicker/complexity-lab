#include "checkpoint.hpp"

#ifdef CLAB_USE_OPENSSL
#include <openssl/evp.h>
#include <memory>
#else
#include <CommonCrypto/CommonDigest.h>
#endif

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace clab {
namespace {
class OpenCheckpoint {
public:
    explicit OpenCheckpoint(const std::filesystem::path& path)
        : fd_(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)) {
        if (fd_ < 0) {
            throw Error(ErrorCode::io, "could not open checkpoint target");
        }
    }

    ~OpenCheckpoint() { ::close(fd_); }
    OpenCheckpoint(const OpenCheckpoint&) = delete;
    OpenCheckpoint& operator=(const OpenCheckpoint&) = delete;

    void check_size(std::uint64_t expected) const {
        struct stat info{};
        if (::fstat(fd_, &info) != 0) {
            throw Error(ErrorCode::io, "could not stat checkpoint target");
        }
        if (!S_ISREG(info.st_mode)) {
            throw Error(ErrorCode::invalid, "checkpoint target must be a regular file");
        }
        if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) != expected) {
            throw Error(ErrorCode::invalid, "checkpoint size mismatch");
        }
    }

    [[nodiscard]] std::size_t read(std::span<std::uint8_t> buffer) const {
        ssize_t count;
        do {
            count = ::read(fd_, buffer.data(), buffer.size());
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            throw Error(ErrorCode::io, "checkpoint read failed");
        }
        return static_cast<std::size_t>(count);
    }

private:
    int fd_;
};

void check_digest_operation(int result) {
    if (result != 1) {
        throw Error(ErrorCode::io, "SHA-256 computation failed");
    }
}

#ifdef CLAB_USE_OPENSSL
class Sha256 {
public:
    Sha256() {
        if (!context_) {
            throw Error(ErrorCode::io, "could not allocate a SHA-256 context");
        }
        check_digest_operation(EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr));
    }
    void update(std::span<const std::uint8_t> bytes) {
        check_digest_operation(EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()));
    }
    [[nodiscard]] std::array<std::uint8_t, 32> finish() {
        std::array<std::uint8_t, EVP_MAX_MD_SIZE> output{};
        unsigned length{};
        check_digest_operation(EVP_DigestFinal_ex(context_.get(), output.data(), &length));
        std::array<std::uint8_t, 32> result{};
        if (length != result.size()) {
            throw Error(ErrorCode::io, "unexpected SHA-256 output length");
        }
        std::copy_n(output.begin(), result.size(), result.begin());
        return result;
    }

private:
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_{EVP_MD_CTX_new(), EVP_MD_CTX_free};
};
#else
class Sha256 {
public:
    Sha256() { check_digest_operation(CC_SHA256_Init(&context_)); }
    void update(std::span<const std::uint8_t> bytes) {
        check_digest_operation(CC_SHA256_Update(&context_, bytes.data(),
                                                static_cast<CC_LONG>(bytes.size())));
    }
    [[nodiscard]] std::array<std::uint8_t, 32> finish() {
        std::array<std::uint8_t, 32> result{};
        check_digest_operation(CC_SHA256_Final(result.data(), &context_));
        return result;
    }

private:
    CC_SHA256_CTX context_{};
};
#endif
}

void verify_checkpoint_target(const CheckpointRef& ref, const std::filesystem::path& path) {
    const OpenCheckpoint target(path);
    target.check_size(ref.size_bytes);

    Sha256 sha;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    auto remaining = ref.size_bytes;
    while (remaining != 0) {
        const auto limit = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        const auto count = target.read(std::span(buffer).first(limit));
        if (count == 0) {
            throw Error(ErrorCode::invalid, "checkpoint ended before declared size");
        }
        sha.update(std::span(buffer).first(count));
        remaining -= count;
    }
    if (target.read(std::span(buffer).first(1)) != 0) {
        throw Error(ErrorCode::invalid, "checkpoint exceeds declared size");
    }
    if (sha.finish() != ref.sha256) {
        throw Error(ErrorCode::invalid, "checkpoint SHA-256 mismatch");
    }
}
}
