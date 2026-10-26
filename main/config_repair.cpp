#include "sdkconfig.h"
#if defined(CONFIG_TESLA_CONFIG_REPAIR_USB) && CONFIG_TESLA_CONFIG_REPAIR_USB
#include "config_repair.hpp"
#include "config_repair_store.hpp"
#include "ota_update.hpp"
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace tk::repair {
namespace {
[[noreturn]] void park() noexcept { for(;;) vTaskDelay(pdMS_TO_TICKS(10000)); }
std::uint64_t deadline(std::uint64_t interval) noexcept {
    return static_cast<std::uint64_t>(esp_timer_get_time())+interval;
}
void report(Console& io,const Receipt& receipt) noexcept {
    // No credential/key/VIN/record values, digests or SDK text. Binary fixed terminal receipt.
    const std::uint8_t bytes[]={'T','K','R','S','1',static_cast<std::uint8_t>(receipt.outcome),
        static_cast<std::uint8_t>(receipt.write_attempted),static_cast<std::uint8_t>(receipt.commit_confirmed),
        static_cast<std::uint8_t>(receipt.readback_confirmed),static_cast<std::uint8_t>(receipt.retention_confirmed)};
    (void)io.write_exact(bytes,sizeof(bytes),deadline(5000000));
}
[[noreturn]] void finish(Console& io,const Receipt& result,Request& request,
                         Wiped<kMaxFrame>& frame) noexcept {
    clear_request(request);
    volatile std::uint8_t* bytes=frame.bytes.data();
    for(std::size_t i=0;i<frame.bytes.size();++i)bytes[i]=0;
    report(io,result);park();
}
}
[[noreturn]] void run(Console& io) noexcept {
    // Static memory exists in this explicit variant only, not the normal build. Its actual
    // enabled-target BSS/frame/heap/flash limits are unmeasured and must be gated independently.
    static IdfStore store;
    static Wiped<kMaxFrame> frame;
    static Request request;
    static Attempt once;
    Receipt result;
    try {
        OtaConfigRestartGuard owner;
        if(!owner.try_begin()) {finish(io,result,request,frame);}
        const auto raw=ota_config_image_state(owner);
        if(raw!=ConfigImageResult::Valid && raw!=ConfigImageResult::Undefined) {finish(io,result,request,frame);}
        if(psa_crypto_init()!=PSA_SUCCESS || !store.open_existing()) {finish(io,result,request,frame);}
        Evidence evidence;
        evidence.explicitly_selected_maintenance=true;
        // This literal is earned only by the lead's mandatory structural early-entry contract.
        // A late, runtime-selectable or network-triggered callsite is not an admissible integration.
        evidence.before_normal_writers=true;
        evidence.actual_restart_owner=owner.owns_gate();
        evidence.raw_image_stable=true;
        evidence.storage_authoritative=true;
        evidence.identity_valid=store.public_identity(evidence.board,evidence.public_key);
        evidence.vin_journal=store.journal(nvs_contract::kVinTransition,true);
        evidence.key_journal=store.journal(nvs_contract::kKeyRotation,false);
        if(!evidence.identity_valid || evidence.vin_journal!=Read::Missing ||
            evidence.key_journal!=Read::Missing) {finish(io,result,request,frame);}
        esp_fill_random(evidence.challenge.data(),evidence.challenge.size());
        std::uint8_t info[5+6+65+32]={'T','K','C','R','1'};
        std::memcpy(info+5,evidence.board.data(),6);
        std::memcpy(info+11,evidence.public_key.data(),65);
        std::memcpy(info+76,evidence.challenge.data(),32);
        if(!io.write_exact(info,sizeof(info),deadline(5000000)))finish(io,result,request,frame);
        const auto until=deadline(30000000); // one absolute 30-second request deadline
        if(!io.read_exact(frame.bytes.data(),kFramePrefix,until))finish(io,result,request,frame);
        const auto size=std::size_t(frame.bytes[5])|(std::size_t(frame.bytes[6])<<8);
        if(size<kFrameFixed || size>frame.bytes.size() ||
           !io.read_exact(frame.bytes.data()+kFramePrefix,size-kFramePrefix,until) ||
           !decode_request(frame.bytes.data(),size,request)) {finish(io,result,request,frame);}
        // Refresh authoritative identity/journal facts immediately before the one write path.
        // No normal task, service or configuration reader is permitted to coexist with this entry.
        evidence.identity_valid=store.public_identity(evidence.board,evidence.public_key);
        evidence.vin_journal=store.journal(nvs_contract::kVinTransition,true);
        evidence.key_journal=store.journal(nvs_contract::kKeyRotation,false);
        const auto fresh=ota_config_image_state(owner);
        evidence.actual_restart_owner=owner.owns_gate();
        evidence.raw_image_stable=fresh==ConfigImageResult::Valid || fresh==ConfigImageResult::Undefined;
        result=repair_once(once,store,owner,request,evidence);
        // On any post-write outcome the actual owner was sealed before nvs_set_blob. A reply
        // failure cannot release it, restart, re-enter startup or permit a second APPLY.
        finish(io,result,request,frame);
    } catch(...) {
        finish(io,result,request,frame);
    }
}
} // namespace tk::repair
#endif
