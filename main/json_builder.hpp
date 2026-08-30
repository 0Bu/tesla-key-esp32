#pragma once

#include <cJSON.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace tk {

struct JsonDelete {
    void operator()(cJSON* value) const noexcept { cJSON_Delete(value); }
};

using JsonOwner = std::unique_ptr<cJSON, JsonDelete>;

inline JsonOwner json_owner(cJSON* value) noexcept { return JsonOwner(value); }

enum class JsonRoot {
    Object,
    Array,
};

// Sticky, ownership-safe cJSON construction. Every Create/Add failure invalidates the complete
// tree; finish() then deletes it in one place. Keeping it alive until finish() lets bounded
// emitters safely retain parent pointers while subsequent no-op emits unwind their fixed stack.
// Callers can pass release() straight to send_json/send_rpc: nullptr means a fail-closed 503,
// while a partially built 200 response can never escape.
class JsonBuilder {
public:
    explicit JsonBuilder(JsonRoot kind = JsonRoot::Object) noexcept
        : root_(kind == JsonRoot::Object ? cJSON_CreateObject() : cJSON_CreateArray()),
          failed_(root_ == nullptr) {}

    JsonBuilder(const JsonBuilder&) = delete;
    JsonBuilder& operator=(const JsonBuilder&) = delete;
    JsonBuilder(JsonBuilder&&) = default;
    JsonBuilder& operator=(JsonBuilder&&) = default;

    bool ok() const noexcept { return !failed_ && root_ != nullptr; }
    cJSON* root() const noexcept { return ok() ? root_.get() : nullptr; }
    void invalidate() noexcept { fail_(); }

    cJSON* object(cJSON* parent, const char* key) noexcept {
        return add_object_(parent, key, cJSON_CreateObject);
    }

    cJSON* array(cJSON* parent, const char* key) noexcept {
        return add_object_(parent, key, cJSON_CreateArray);
    }

    bool string(cJSON* parent, const char* key, const char* value) noexcept {
        if (failed_) return false;
        return adopt_object(parent, key, json_owner(cJSON_CreateString(value))) != nullptr;
    }

    bool string_reference(cJSON* parent, const char* key, const char* value) noexcept {
        if (failed_) return false;
        return adopt_object(parent, key, json_owner(cJSON_CreateStringReference(value))) != nullptr;
    }

    bool number(cJSON* parent, const char* key, double value) noexcept {
        if (failed_) return false;
        return adopt_object(parent, key, json_owner(cJSON_CreateNumber(value))) != nullptr;
    }

    // cJSON's double printer may round a JSON-safe integer such as 2^53-1 into exponential text
    // that reparses to a neighbouring value. CreateRaw is safe here only because this method
    // constructs the complete canonical base-10 integer itself; callers cannot inject raw JSON.
    bool integer(cJSON* parent, const char* key, int64_t value) noexcept {
        if (failed_) return false;
        char text[22]{};  // sign + 20 uint64 digits + NUL
        char reverse[20]{};
        uint64_t magnitude = value < 0
            ? static_cast<uint64_t>(-(value + 1)) + 1u
            : static_cast<uint64_t>(value);
        size_t digits = 0;
        do {
            reverse[digits++] = static_cast<char>('0' + magnitude % 10u);
            magnitude /= 10u;
        } while (magnitude != 0);
        size_t out = 0;
        if (value < 0) text[out++] = '-';
        while (digits != 0) text[out++] = reverse[--digits];
        text[out] = '\0';
        return adopt_object(parent, key, json_owner(cJSON_CreateRaw(text))) != nullptr;
    }

    bool boolean(cJSON* parent, const char* key, bool value) noexcept {
        if (failed_) return false;
        return adopt_object(parent, key, json_owner(cJSON_CreateBool(value))) != nullptr;
    }

    bool null(cJSON* parent, const char* key) noexcept {
        if (failed_) return false;
        return adopt_object(parent, key, json_owner(cJSON_CreateNull())) != nullptr;
    }

    bool element(cJSON* array, JsonOwner child) noexcept {
        if (failed_ || !array || !child || !cJSON_IsArray(array)) {
            fail_();
            return false;
        }
        if (!cJSON_AddItemToArray(array, child.get())) {
            fail_();
            return false;
        }
        child.release();
        return true;
    }

    cJSON* object_element(cJSON* array) noexcept {
        if (failed_) return nullptr;
        JsonOwner child(cJSON_CreateObject());
        cJSON* raw = child.get();
        return element(array, std::move(child)) ? raw : nullptr;
    }

    cJSON* array_element(cJSON* array) noexcept {
        if (failed_) return nullptr;
        JsonOwner child(cJSON_CreateArray());
        cJSON* raw = child.get();
        return element(array, std::move(child)) ? raw : nullptr;
    }

    bool string_element(cJSON* array, const char* value) noexcept {
        if (failed_) return false;
        return element(array, json_owner(cJSON_CreateString(value)));
    }

    bool string_reference_element(cJSON* array, const char* value) noexcept {
        if (failed_) return false;
        return element(array, json_owner(cJSON_CreateStringReference(value)));
    }

    bool number_element(cJSON* array, double value) noexcept {
        if (failed_) return false;
        return element(array, json_owner(cJSON_CreateNumber(value)));
    }

    bool boolean_element(cJSON* array, bool value) noexcept {
        if (failed_) return false;
        return element(array, json_owner(cJSON_CreateBool(value)));
    }

    bool null_element(cJSON* array) noexcept {
        if (failed_) return false;
        return element(array, json_owner(cJSON_CreateNull()));
    }

    // Consumes child on both success and failure. Ownership is released only after cJSON confirms
    // adoption; an add-key allocation failure therefore cannot leak the detached MCP id/result.
    cJSON* adopt_object(cJSON* parent, const char* key, JsonOwner child) noexcept {
        if (failed_ || !parent || !key || !child || cJSON_IsArray(parent)) {
            fail_();
            return nullptr;
        }
        cJSON* raw = child.get();
        if (!cJSON_AddItemToObject(parent, key, raw)) {
            fail_();
            return nullptr;
        }
        child.release();
        return raw;
    }

    JsonOwner finish() noexcept {
        if (failed_) {
            root_.reset();
            return {};
        }
        return std::move(root_);
    }

    cJSON* release() noexcept { return finish().release(); }

private:
    using Create = cJSON* (*)();

    cJSON* add_object_(cJSON* parent, const char* key, Create create) noexcept {
        if (failed_) return nullptr;
        JsonOwner child(create());
        cJSON* raw = child.get();
        return adopt_object(parent, key, std::move(child)) ? raw : nullptr;
    }

    void fail_() noexcept {
        failed_ = true;
    }

    JsonOwner root_;
    bool      failed_{false};
};

}  // namespace tk
