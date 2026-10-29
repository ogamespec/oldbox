#pragma once
#include "oldbox/common.hpp"
#include <deque>
#include <type_traits>

namespace oldbox {

class Machine;

// The on-disk save-state format is deliberately simple: a fixed header with
// a config fingerprint and payload CRC, followed by the payload written in
// little-endian host order.  Payloads only contain trivially copyable values
// and explicit container element streams, so a state is independent of host
// pointer values and std::function callbacks.
constexpr u32 state_version = 3;
constexpr char state_magic[8] = {'O', 'L', 'D', 'B', 'O', 'X', 'S', 'T'};

struct StateHeader {
    char magic[8];
    u32 version;
    u32 header_size;
    u32 config_hash;
    u32 payload_crc;
    u64 payload_size;
};

class StateWriter {
public:
    StateWriter() { data_.reserve(1u << 20); }

    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const u8*>(p);
        data_.insert(data_.end(), b, b + n);
    }
    template <typename T>
    void pod(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "save state pod() requires a POD type");
        bytes(&value, sizeof(T));
    }
    template <typename T>
    void vec(const std::vector<T>& value) {
        pod<u64>(u64(value.size()));
        if constexpr (std::is_trivially_copyable_v<T>) {
            if (!value.empty()) bytes(value.data(), value.size() * sizeof(T));
        } else {
            for (const T& item : value) pod(item);
        }
    }
    template <typename T, std::size_t N>
    void array(const std::array<T, N>& value) {
        if constexpr (std::is_trivially_copyable_v<T>) {
            bytes(value.data(), value.size() * sizeof(T));
        } else {
            for (const T& item : value) pod(item);
        }
    }
    template <typename T, std::size_t N>
    void array(const T (&value)[N]) {
        static_assert(std::is_trivially_copyable_v<T>, "save state C-array requires a POD type");
        bytes(value, sizeof(T) * N);
    }
    template <typename T>
    void deque(const std::deque<T>& value) {
        static_assert(std::is_trivially_copyable_v<T>, "save state deque() requires a POD type");
        pod<u64>(u64(value.size()));
        for (const T& item : value) pod(item);
    }
    void str(const std::string& value) {
        pod<u64>(u64(value.size()));
        if (!value.empty()) bytes(value.data(), value.size());
    }

    [[nodiscard]] const std::vector<u8>& data() const { return data_; }
    [[nodiscard]] std::vector<u8>& data() { return data_; }
    [[nodiscard]] std::size_t size() const { return data_.size(); }

private:
    std::vector<u8> data_;
};

class StateReader {
public:
    explicit StateReader(std::span<const u8> data) : data_(data) {}

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::size_t remaining() const { return ok_ ? data_.size() - pos_ : 0; }

    void bytes(void* p, std::size_t n) {
        if (!ok_ || n > data_.size() - pos_) {
            ok_ = false;
            throw Error("truncated save state");
        }
        std::memcpy(p, data_.data() + pos_, n);
        pos_ += n;
    }
    template <typename T>
    void pod(T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "save state pod() requires a POD type");
        bytes(&value, sizeof(T));
    }
    template <typename T>
    void vec(std::vector<T>& value) {
        u64 n = 0;
        pod(n);
        if (n > data_.size() - pos_) {
            ok_ = false;
            throw Error("invalid save-state container size");
        }
        value.resize(std::size_t(n));
        if constexpr (std::is_trivially_copyable_v<T>) {
            if (!value.empty()) bytes(value.data(), value.size() * sizeof(T));
        } else {
            for (T& item : value) pod(item);
        }
    }
    template <typename T, std::size_t N>
    void array(std::array<T, N>& value) {
        if constexpr (std::is_trivially_copyable_v<T>) {
            bytes(value.data(), value.size() * sizeof(T));
        } else {
            for (T& item : value) pod(item);
        }
    }
    template <typename T, std::size_t N>
    void array(T (&value)[N]) {
        static_assert(std::is_trivially_copyable_v<T>, "save state C-array requires a POD type");
        bytes(value, sizeof(T) * N);
    }
    template <typename T>
    void deque(std::deque<T>& value) {
        static_assert(std::is_trivially_copyable_v<T>, "save state deque() requires a POD type");
        u64 n = 0;
        pod(n);
        if (n > data_.size() - pos_) {
            ok_ = false;
            throw Error("invalid save-state container size");
        }
        value.clear();
        for (u64 i = 0; i < n; ++i) {
            T item{};
            pod(item);
            value.push_back(item);
        }
    }
    void str(std::string& value) {
        u64 n = 0;
        pod(n);
        if (n > data_.size() - pos_) {
            ok_ = false;
            throw Error("invalid save-state string size");
        }
        value.resize(std::size_t(n));
        if (n) bytes(value.data(), std::size_t(n));
    }

private:
    std::span<const u8> data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

// Save/load the complete machine state.  Both functions throw oldbox::Error
// with a user-readable message when the file cannot be used.
void save_machine_state(const Machine& machine, const std::string& path);
void load_machine_state(Machine& machine, const std::string& path);

} // namespace oldbox
