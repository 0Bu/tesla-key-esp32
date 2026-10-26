#include "ota_coordinator.hpp"
#include <algorithm>
#include <cstring>

namespace tk::ota {
Coordinator::Coordinator(Slot& slot,Job& job,Budget& budget,CoordinatorClock clock,Effects effects,std::uint64_t image_budget_overhead) noexcept
    : slot_(slot),job_(job),budget_(budget),clock_(clock),effects_(effects),image_budget_overhead_(image_budget_overhead) {}
bool Coordinator::alive() noexcept {
    return !job_.retired() && budget_.live(clock_.now_ms(clock_.context));
}
bool Coordinator::start(const char* url,HttpLimits limits,bool image,
                         BodyCallback sink,void* context,bool optional_metadata) noexcept {
    if (!slot_.owns(job_) || lease_released_ || budget_.started() != job_.started_ms ||
        budget_.deadline() > job_.deadline_ms ||
        (outcome_ != Outcome::Idle && outcome_ != Outcome::MetadataReady) || !alive() ||
        !effects_.check_prefix || !effects_.begin || !effects_.write || !effects_.end ||
        !effects_.select_boot || !effects_.abort || !effects_.quiescent || !effects_.release_lease ||
        (image && (!effects_.prefix_bytes || effects_.prefix_bytes > prefix_.size()))) return false;
    http_parser_url parsed{};
    if (!valid_https_url(url,parsed)) return false;
    image_ = image; limits_ = limits; metadata_sink_ = sink; metadata_context_ = context;
    optional_metadata_ = optional_metadata && !image; metadata_failed_ = false;
    prefix_size_ = redirects_ = 0; begun_ = borrowed_ = redirect_wait_ = tightened_ = false;
    framing_.emplace(limits,receive,this);
    if (framing_->result() != ParseResult::More || !job_.request(url)) return false;
    outcome_ = Outcome::Working;
    return true;
}
bool Coordinator::receive(void* context,const std::uint8_t* bytes,std::size_t n) noexcept {
    return static_cast<Coordinator*>(context)->body(bytes,n);
}
bool Coordinator::body(const std::uint8_t* bytes,std::size_t n) noexcept {
    if (image_ && framing_->has_declared_size() && !tightened_) {
        tightened_ = true;
        if (!budget_.tighten(framing_->declared_size() + image_budget_overhead_)) { fail(Outcome::ProtocolError); return false; }
    }
    if (!alive()) { fail(Outcome::Deadline); return false; }
    if (!image_) return !metadata_sink_ || metadata_sink_(metadata_context_,bytes,n);
    if (!begun_) {
        const auto take = std::min(n,effects_.prefix_bytes-prefix_size_);
        std::memcpy(prefix_.data()+prefix_size_,bytes,take);
        prefix_size_ += take; bytes += take; n -= take;
        if (prefix_size_ != effects_.prefix_bytes) return true;
        if (!alive()) { fail(Outcome::Deadline); return false; }
        const auto admit = effects_.check_prefix(effects_.context,prefix_.data(),prefix_size_);
        if (admit != Effect::Ok) { fail(Outcome::ProtocolError); return false; }
        if (!alive()) { fail(Outcome::Deadline); return false; }
        const auto begin = effects_.begin(effects_.context);
        if (begin != Effect::Ok) {
            fail(begin == Effect::NoMemory ? Outcome::NoMemory : Outcome::WriterError); return false;
        }
        begun_ = true;
        if (!alive()) { fail(Outcome::Deadline); return false; }
        const auto prefix_write = effects_.write(effects_.context,prefix_.data(),prefix_size_);
        if (prefix_write != Effect::Ok) {
            fail(prefix_write == Effect::NoMemory ? Outcome::NoMemory : Outcome::WriterError);
            return false;
        }
        if (!alive()) { fail(Outcome::Deadline); return false; }
    }
    if (n) {
        const auto payload_write = effects_.write(effects_.context,bytes,n);
        if (payload_write != Effect::Ok) {
            fail(payload_write == Effect::NoMemory ? Outcome::NoMemory : Outcome::WriterError);
            return false;
        }
    }
    if (!alive()) { fail(Outcome::Deadline); return false; }
    return true;
}
void Coordinator::release_if_safe() noexcept {
    if (lease_released_ || outcome_ == Outcome::Committed || outcome_ == Outcome::WriterUncertain ||
        borrowed_ || !effects_.quiescent(effects_.context)) return;
    lease_released_ = effects_.release_lease(effects_.context);
    if (!lease_released_) outcome_ = Outcome::WriterUncertain;
}
void Coordinator::fail(Outcome reason) noexcept {
    if (outcome_ == Outcome::Committed || outcome_ == Outcome::WriterUncertain) return;
    if (optional_metadata_ && !image_ && !job_.retired() &&
        (reason == Outcome::NetworkError || reason == Outcome::ProtocolError) &&
        budget_.live(clock_.now_ms(clock_.context))) {
        metadata_failed_ = true;
        job_.close_connection();
        outcome_ = Outcome::MetadataReady;
        return; // Same budget/lease; caller must await real Closed before another leg.
    }
    job_.retire();
    outcome_ = reason;
    if (!effects_.quiescent(effects_.context) && !effects_.abort(effects_.context))
        outcome_ = Outcome::WriterUncertain;
    release_if_safe();
}
void Coordinator::cancel(Outcome reason) noexcept { fail(reason); }
void Coordinator::finish_image() noexcept {
    if (!begun_ || !alive()) { fail(begun_ ? Outcome::Deadline : Outcome::ProtocolError); return; }
    const auto end = effects_.end(effects_.context); // Consumes raw handle on every return.
    if (end != Effect::Ok) { fail(end == Effect::NoMemory ? Outcome::NoMemory : Outcome::WriterError); return; }
    if (!alive()) { fail(Outcome::Deadline); return; }
    const auto selection = effects_.select_boot(effects_.context);
    // Selection is a durable-effect boundary. Neither success nor uncertain return may release
    // authority because a later clock sample is beyond the deadline.
    job_.retire();
    if (selection == Effect::Committed) outcome_ = Outcome::Committed;
    else if (selection == Effect::Uncertain) outcome_ = Outcome::WriterUncertain;
    else fail(Outcome::WriterError);
}
Outcome Coordinator::poll() noexcept {
    if (outcome_ == Outcome::Committed || outcome_ == Outcome::WriterUncertain || lease_released_)
        return outcome_;
    if (!alive()) { fail(Outcome::Deadline); return outcome_; }
    if (job_.phase() == Phase::CleanupFailed) {
        optional_metadata_ = false; fail(Outcome::NetworkError); return outcome_;
    }
    if (outcome_ != Outcome::Working) return outcome_;
    if (redirect_wait_) {
        if (job_.phase() != Phase::Closed) return outcome_;
        if (!job_.request(redirect_.data())) { fail(Outcome::InvalidState); return outcome_; }
        framing_.emplace(limits_,receive,this);
        redirect_wait_ = false;
    }
    if (!job_.borrow()) return outcome_;
    borrowed_ = true;
    ParseResult parse = ParseResult::More;
    const unsigned headers_before = framing_->status();
    if (!alive()) fail(Outcome::Deadline);
    else if (job_.result == NetResult::Data && job_.bytes && job_.bytes <= job_.block.size())
        parse = framing_->feed(job_.block.data(),job_.bytes);
    else if (job_.result == NetResult::Eof) parse = framing_->eof();
    else fail(job_.result == NetResult::NoMemory ? Outcome::NoMemory :
              job_.result == NetResult::Deadline ? Outcome::Deadline : Outcome::NetworkError);
    // status is assigned by the real headers-complete callback, never by the first read.
    // feed may also consume the first body chunk; this is not an atomic parser/heap peak.
    if (headers_before < 200 && framing_->status() >= 200)
        memory_checkpoint("headers_observed", job_.generation, job_.started_ms);
    if (outcome_ == Outcome::Working) {
        if (image_ && framing_->status() == 200 && framing_->has_declared_size() && !tightened_) {
            tightened_ = true;
            if (!budget_.tighten(framing_->declared_size() + image_budget_overhead_)) fail(Outcome::ProtocolError);
        }
        if (!alive()) fail(Outcome::Deadline);
        if (outcome_ == Outcome::Working && parse == ParseResult::Redirect) {
            if (redirects_ >= 10 || !resolve_redirect(job_.url.data(),framing_->location(),redirect_))
                fail(Outcome::ProtocolError);
            else { ++redirects_; redirect_wait_ = true; job_.close_connection(); }
        } else if (outcome_ == Outcome::Working && parse == ParseResult::Complete) {
            job_.close_connection();
            if (image_) finish_image();
            else outcome_ = Outcome::MetadataReady;
        } else if (outcome_ == Outcome::Working &&
                   (parse == ParseResult::Rejected || parse == ParseResult::SinkFailed))
            fail(Outcome::ProtocolError);
    }
    if (!job_.release()) {
        outcome_ = Outcome::WriterUncertain; job_.retire();
    }
    borrowed_ = false;
    if (outcome_ != Outcome::Working && outcome_ != Outcome::MetadataReady) release_if_safe();
    return outcome_;
}
void Coordinator::finish_metadata_operation() noexcept {
    if (outcome_ != Outcome::MetadataReady || !alive()) { fail(Outcome::Deadline); return; }
    job_.retire();
    release_if_safe();
}
bool Coordinator::can_reclaim() const noexcept {
    return lease_released_ && !borrowed_ && worker_clean(job_) && effects_.quiescent(effects_.context);
}
} // namespace tk::ota
