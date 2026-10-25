#pragma once

#if !defined(__linux__)
#error "Cito POSIX SHM ring prototype currently requires Linux"
#endif

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace cito::detail {

namespace shm_detail {

inline constexpr std::uint64_t kMagic =
    0x4349544f53484d31ULL; // CITOSHM1
inline constexpr std::size_t kCacheLine = 64;

inline std::size_t align_up(
    std::size_t value,
    std::size_t alignment) {
    return
        (value + alignment - 1) /
        alignment *
        alignment;
}

struct alignas(kCacheLine) RingHeader {
    std::uint64_t magic{};
    std::uint32_t version{};
    std::uint32_t slot_count{};
    std::uint32_t slot_size{};
    std::uint32_t slot_stride{};
    std::uint64_t generation{};

    alignas(kCacheLine)
    std::atomic<std::uint64_t>
        write_sequence{0};
};

struct alignas(16) SlotHeader {
    // Even value: complete sequence << 1.
    // Odd value: writer is currently replacing the slot.
    std::atomic<std::uint64_t> stamp{0};
    std::uint32_t length{};
    std::uint32_t reserved{};
};

static_assert(
    std::atomic<std::uint64_t>::
        is_always_lock_free,
    "Cito SHM requires lock-free 64-bit atomics");

inline std::size_t stride_for(
    std::size_t slot_size) {
    return align_up(
        sizeof(SlotHeader) +
            slot_size,
        kCacheLine);
}

inline std::size_t region_size(
    std::size_t slot_count,
    std::size_t slot_size) {
    return
        align_up(
            sizeof(RingHeader),
            kCacheLine) +
        slot_count *
            stride_for(slot_size);
}

inline SlotHeader* slot_at(
    void* base,
    const RingHeader& header,
    std::size_t index) {
    auto* bytes =
        static_cast<std::byte*>(base);

    const auto offset =
        align_up(
            sizeof(RingHeader),
            kCacheLine) +
        index *
            header.slot_stride;

    return reinterpret_cast<
        SlotHeader*>(
            bytes + offset);
}

inline const SlotHeader* slot_at(
    const void* base,
    const RingHeader& header,
    std::size_t index) {
    const auto* bytes =
        static_cast<
            const std::byte*>(base);

    const auto offset =
        align_up(
            sizeof(RingHeader),
            kCacheLine) +
        index *
            header.slot_stride;

    return reinterpret_cast<
        const SlotHeader*>(
            bytes + offset);
}

inline std::byte* payload_at(
    SlotHeader* slot) {
    return
        reinterpret_cast<std::byte*>(
            slot) +
        sizeof(SlotHeader);
}

inline const std::byte* payload_at(
    const SlotHeader* slot) {
    return
        reinterpret_cast<
            const std::byte*>(
                slot) +
        sizeof(SlotHeader);
}

inline std::runtime_error sys_error(
    const char* operation) {
    return std::runtime_error(
        std::string(operation) +
        ": " +
        std::strerror(errno));
}

} // namespace shm_detail

class ShmRingWriter {
public:
    ShmRingWriter(
        std::string name,
        std::uint32_t slot_count,
        std::uint32_t slot_size,
        std::uint64_t generation)
        : name_(std::move(name)) {
        if (
            name_.empty() ||
            name_[0] != '/') {
            throw std::invalid_argument(
                "Cito SHM name must start with '/'");
        }

        if (
            slot_count == 0 ||
            slot_size == 0) {
            throw std::invalid_argument(
                "Cito SHM slot count/size must be non-zero");
        }

        const auto stride =
            shm_detail::stride_for(
                slot_size);

        if (stride >
            static_cast<std::size_t>(
                UINT32_MAX)) {
            throw std::length_error(
                "Cito SHM slot stride exceeds uint32");
        }

        size_ =
            shm_detail::region_size(
                slot_count,
                slot_size);

        fd_ = ::shm_open(
            name_.c_str(),
            O_CREAT |
                O_EXCL |
                O_RDWR,
            0600);

        if (fd_ < 0) {
            throw
                shm_detail::sys_error(
                    "shm_open writer");
        }

        try {
            if (::ftruncate(
                    fd_,
                    static_cast<off_t>(
                        size_)) != 0) {
                throw
                    shm_detail::sys_error(
                        "ftruncate SHM");
            }

            base_ = ::mmap(
                nullptr,
                size_,
                PROT_READ |
                    PROT_WRITE,
                MAP_SHARED,
                fd_,
                0);

            if (base_ ==
                MAP_FAILED) {
                base_ = nullptr;
                throw
                    shm_detail::sys_error(
                        "mmap writer");
            }

            std::memset(
                base_,
                0,
                size_);

            header_ =
                new (base_)
                    shm_detail::RingHeader{};

            header_->magic =
                shm_detail::kMagic;
            header_->version = 1;
            header_->slot_count =
                slot_count;
            header_->slot_size =
                slot_size;
            header_->slot_stride =
                static_cast<
                    std::uint32_t>(
                        stride);
            header_->generation =
                generation;

            header_->
                write_sequence.store(
                    0,
                    std::memory_order_relaxed);

            for (std::size_t i = 0;
                 i < slot_count;
                 ++i) {
                auto* slot =
                    shm_detail::slot_at(
                        base_,
                        *header_,
                        i);

                new (slot)
                    shm_detail::SlotHeader{};

                slot->stamp.store(
                    0,
                    std::memory_order_relaxed);
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ShmRingWriter(
        const ShmRingWriter&) = delete;
    ShmRingWriter& operator=(
        const ShmRingWriter&) = delete;

    ~ShmRingWriter() {
        cleanup();
    }

    std::uint64_t publish(
        const void* data,
        std::size_t size) {
        if (size >
            header_->slot_size) {
            throw std::length_error(
                "Cito SHM payload exceeds slot size");
        }

        const auto sequence =
            header_->
                write_sequence.load(
                    std::memory_order_relaxed) +
            1;

        const auto index =
            static_cast<std::size_t>(
                (sequence - 1) %
                header_->slot_count);

        auto* slot =
            shm_detail::slot_at(
                base_,
                *header_,
                index);

        // Readers never publish progress into shared memory.
        // The writer can therefore replace this slot without waiting
        // for slow/dead readers.
        slot->stamp.store(
            (sequence << 1) | 1ULL,
            std::memory_order_release);

        slot->length =
            static_cast<
                std::uint32_t>(
                    size);

        if (size != 0) {
            std::memcpy(
                shm_detail::payload_at(
                    slot),
                data,
                size);
        }

        slot->stamp.store(
            sequence << 1,
            std::memory_order_release);

        header_->
            write_sequence.store(
                sequence,
                std::memory_order_release);

        return sequence;
    }

    std::uint64_t publish(
        std::string_view value) {
        return publish(
            value.data(),
            value.size());
    }

    std::uint64_t generation()
        const noexcept {
        return header_->generation;
    }

    std::uint32_t slot_count()
        const noexcept {
        return header_->slot_count;
    }

    std::uint32_t slot_size()
        const noexcept {
        return header_->slot_size;
    }

private:
    void cleanup() noexcept {
        if (base_) {
            ::munmap(
                base_,
                size_);
            base_ = nullptr;
            header_ = nullptr;
        }

        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }

        if (!name_.empty()) {
            ::shm_unlink(
                name_.c_str());
        }
    }

    std::string name_;
    int fd_{-1};
    void* base_{nullptr};
    std::size_t size_{};
    shm_detail::RingHeader*
        header_{nullptr};
};

class ShmRingReader {
public:
    explicit ShmRingReader(
        std::string name)
        : name_(std::move(name)) {
        fd_ = ::shm_open(
            name_.c_str(),
            O_RDONLY,
            0);

        if (fd_ < 0) {
            throw
                shm_detail::sys_error(
                    "shm_open reader");
        }

        struct stat status{};

        if (::fstat(
                fd_,
                &status) != 0) {
            cleanup();
            throw
                shm_detail::sys_error(
                    "fstat SHM");
        }

        size_ =
            static_cast<std::size_t>(
                status.st_size);

        base_ = ::mmap(
            nullptr,
            size_,
            PROT_READ,
            MAP_SHARED,
            fd_,
            0);

        if (base_ ==
            MAP_FAILED) {
            base_ = nullptr;
            cleanup();
            throw
                shm_detail::sys_error(
                    "mmap reader");
        }

        header_ =
            static_cast<
                const shm_detail::
                    RingHeader*>(
                        base_);

        if (
            header_->magic !=
                shm_detail::kMagic ||
            header_->version != 1 ||
            header_->slot_count == 0 ||
            header_->slot_size == 0 ||
            header_->slot_stride !=
                shm_detail::stride_for(
                    header_->slot_size) ||
            size_ !=
                shm_detail::region_size(
                    header_->slot_count,
                    header_->slot_size)) {
            cleanup();
            throw std::runtime_error(
                "Cito SHM invalid ring header");
        }
    }

    ShmRingReader(
        const ShmRingReader&) = delete;
    ShmRingReader& operator=(
        const ShmRingReader&) = delete;

    ~ShmRingReader() {
        cleanup();
    }

    bool try_read_next(
        std::vector<std::uint8_t>& out,
        std::uint64_t& dropped) {
        const auto current =
            header_->
                write_sequence.load(
                    std::memory_order_acquire);

        if (current <
            next_sequence_) {
            return false;
        }

        const auto earliest =
            current >=
                    header_->slot_count
                ? current -
                      header_->slot_count +
                      1
                : 1;

        if (next_sequence_ <
            earliest) {
            dropped +=
                earliest -
                next_sequence_;
            next_sequence_ =
                earliest;
        }

        const auto sequence =
            next_sequence_;
        const auto index =
            static_cast<std::size_t>(
                (sequence - 1) %
                header_->slot_count);

        const auto* slot =
            shm_detail::slot_at(
                base_,
                *header_,
                index);

        const auto expected =
            sequence << 1;

        const auto before =
            slot->stamp.load(
                std::memory_order_acquire);

        if (
            before != expected ||
            (before & 1ULL) != 0) {
            return false;
        }

        const auto length =
            slot->length;

        if (length >
            header_->slot_size) {
            throw std::runtime_error(
                "Cito SHM invalid slot length");
        }

        out.resize(length);

        if (length != 0) {
            std::memcpy(
                out.data(),
                shm_detail::payload_at(
                    slot),
                length);
        }

        const auto after =
            slot->stamp.load(
                std::memory_order_acquire);

        if (after != before) {
            out.clear();
            return false;
        }

        ++next_sequence_;
        return true;
    }

    std::uint64_t generation()
        const noexcept {
        return header_->generation;
    }

    std::uint64_t next_sequence()
        const noexcept {
        return next_sequence_;
    }

private:
    void cleanup() noexcept {
        if (base_) {
            ::munmap(
                base_,
                size_);
            base_ = nullptr;
            header_ = nullptr;
        }

        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    std::string name_;
    int fd_{-1};
    void* base_{nullptr};
    std::size_t size_{};
    const shm_detail::RingHeader*
        header_{nullptr};
    std::uint64_t next_sequence_{1};
};

} // namespace cito::detail
