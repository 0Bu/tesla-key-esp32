#pragma once
#include "logic/ota_lifetime.hpp"
#include "ota_http.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace tk::ota {
enum class Effect : std::uint8_t { Ok, Failed, NoMemory, Uncertain, Committed };
struct Effects {
    void* context;
    std::size_t prefix_bytes;
    Effect (*check_prefix)(void*,const std::uint8_t*,std::size_t) noexcept;
    Effect (*begin)(void*) noexcept;
    Effect (*write)(void*,const std::uint8_t*,std::size_t) noexcept;
    Effect (*end)(void*) noexcept;
    Effect (*select_boot)(void*) noexcept;
    bool (*abort)(void*) noexcept;
    bool (*quiescent)(void*) noexcept;
    bool (*release_lease)(void*) noexcept; // Actual owner-checked CAS acknowledgement required.
};
enum class Outcome : std::uint8_t {
    Idle, Working, MetadataReady, Committed, Deadline, ProtocolError,
    NetworkError, NoMemory, WriterError, WriterUncertain, InvalidState
};
struct CoordinatorClock { void* context; std::uint64_t (*now_ms)(void*) noexcept; };
class Coordinator {
public:
    Coordinator(Slot&,Job&,Budget&,CoordinatorClock,Effects,
                std::uint64_t image_budget_overhead = 0) noexcept;
    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;
    // The same Budget/Job/generation persist over manifest, changelog and image legs.
    bool start(const char* url, HttpLimits, bool image,
               BodyCallback metadata_sink = nullptr, void* sink_context = nullptr,
               bool optional_metadata = false) noexcept;
    Outcome poll() noexcept; // Caller runs it independently of the network worker.
    void cancel(Outcome reason = Outcome::Deadline) noexcept;
    void finish_metadata_operation() noexcept;
    Outcome outcome() const noexcept { return outcome_; }
    bool writer_lease_released() const noexcept { return lease_released_; }
    bool can_reclaim() const noexcept;
    bool metadata_failed() const noexcept { return metadata_failed_; }
    std::uint64_t body_bytes() const noexcept { return framing_ ? framing_->body_bytes() : 0; }
    std::uint64_t declared_size() const noexcept { return framing_ ? framing_->declared_size() : 0; }
private:
    static bool receive(void*,const std::uint8_t*,std::size_t) noexcept;
    bool body(const std::uint8_t*,std::size_t) noexcept;
    bool alive() noexcept;
    void fail(Outcome) noexcept;
    void release_if_safe() noexcept;
    void finish_image() noexcept;
    Slot& slot_;
    Job& job_;
    Budget& budget_;
    CoordinatorClock clock_;
    Effects effects_;
    std::optional<Framing> framing_{};
    HttpLimits limits_{};
    BodyCallback metadata_sink_{nullptr};
    void* metadata_context_{nullptr};
    std::array<std::uint8_t,512> prefix_{};
    std::array<char,kUrlBytes> redirect_{};
    std::size_t prefix_size_{0};
    unsigned redirects_{0};
    Outcome outcome_{Outcome::Idle};
    bool image_{false}, begun_{false}, borrowed_{false}, lease_released_{false};
    bool redirect_wait_{false}, tightened_{false};
    bool optional_metadata_{false}, metadata_failed_{false};
    const std::uint64_t image_budget_overhead_;
};
} // namespace tk::ota
