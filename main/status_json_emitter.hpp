#pragma once

#include "json_builder.hpp"

#include <cstddef>

namespace tk {

// cJSON visitor for logic/status_model.hpp. The fixed stack mirrors that model's maximum nesting;
// JsonBuilder keeps every parent alive until finish(), even after a sticky allocation failure.
class StatusJsonEmitter {
public:
    static constexpr size_t kStackCapacity = 5;

    explicit StatusJsonEmitter(JsonBuilder& builder) : json_(builder) {
        stack_[0] = json_.root();
    }

    void obj_begin(const char* key) noexcept {
        if (!may_begin_()) return;
        cJSON* parent = top_();
        cJSON* child = cJSON_IsArray(parent) ? json_.object_element(parent)
                                             : json_.object(parent, key);
        stack_[++depth_] = child;
    }

    void obj_end() noexcept { end_(); }

    void arr_begin(const char* key) noexcept {
        if (!may_begin_()) return;
        cJSON* parent = top_();
        cJSON* child = cJSON_IsArray(parent) ? json_.array_element(parent)
                                             : json_.array(parent, key);
        stack_[++depth_] = child;
    }

    void arr_end() noexcept { end_(); }

    void str(const char* key, const char* value) noexcept {
        if (!valid_) return;
        if (cJSON_IsArray(top_())) json_.string_element(top_(), value);
        else                      json_.string(top_(), key, value);
    }

    void num(const char* key, double value) noexcept {
        if (!valid_) return;
        if (cJSON_IsArray(top_())) json_.number_element(top_(), value);
        else                      json_.number(top_(), key, value);
    }

    void boolean(const char* key, bool value) noexcept {
        if (!valid_) return;
        if (cJSON_IsArray(top_())) json_.boolean_element(top_(), value);
        else                      json_.boolean(top_(), key, value);
    }

    JsonOwner finish() noexcept {
        if (!valid_ || depth_ != 0) invalidate_();
        return json_.finish();
    }

    cJSON* release() noexcept { return finish().release(); }

private:
    bool may_begin_() noexcept {
        if (!valid_) return false;
        if (depth_ + 1 >= kStackCapacity) {
            invalidate_();
            return false;
        }
        return true;
    }

    void end_() noexcept {
        if (!valid_) return;
        if (depth_ == 0) {
            invalidate_();
            return;
        }
        stack_[depth_] = nullptr;
        --depth_;
    }

    void invalidate_() noexcept {
        valid_ = false;
        json_.invalidate();
    }

    cJSON* top_() const { return stack_[depth_]; }

    JsonBuilder& json_;
    cJSON*       stack_[kStackCapacity]{};
    size_t       depth_{0};
    bool         valid_{true};
};

}  // namespace tk
