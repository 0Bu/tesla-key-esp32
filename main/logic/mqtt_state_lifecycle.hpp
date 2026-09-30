#pragma once
// Retained-state lifecycle of the HA MQTT bridge's state topics (mqtt_ha.cpp). Pure, IDF-free,
// host-tested.
//
// State topics are published RETAINED, so the broker keeps the last message for as long as nobody
// replaces it. A domain whose RAM cache is no longer valid (pairing reset, invalidated cache,
// nothing heard yet after a reboot) that merely stops publishing therefore leaves the OLD readings
// on the broker, and Home Assistant shows them as current again after every reconnect or restart
// even though /status already reports the values as unknown. The bridge must overwrite them once
// with an empty state object (the presence-aware templates in ha_templates.hpp render every absent
// field as `None`, i.e. unknown) and then stay quiet until the domain is valid again.
#include <cstdint>

namespace tk::mqtt {

enum class DomainPublishState : uint8_t {
    // Nothing published in this run of the bridge. The broker may still hold a retained value from
    // an earlier session or from before a reboot, so an invalid cache must clear it once.
    Unknown,
    // A real payload was published last.
    Published,
    // The retained value was replaced with an empty object; nothing further to do until valid.
    Cleared,
};

enum class DomainPublishAction : uint8_t {
    Publish,       // build and publish the domain's payload
    PublishClear,  // publish the empty object over the retained value
    Skip,          // the broker already reflects the cache
};

struct DomainPublishDecision {
    DomainPublishAction action;
    DomainPublishState next;  // adopt only after the publish was accepted
};

inline constexpr DomainPublishDecision decide_domain_publish(bool cache_valid,
                                                             DomainPublishState previous) noexcept {
    if (cache_valid) return {DomainPublishAction::Publish, DomainPublishState::Published};
    if (previous == DomainPublishState::Cleared) {
        return {DomainPublishAction::Skip, DomainPublishState::Cleared};
    }
    return {DomainPublishAction::PublishClear, DomainPublishState::Cleared};
}

}  // namespace tk::mqtt
