#include "ota_writer.hpp"
#include <sdkconfig.h>
#if !CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT || !CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME || !CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
#error "OTA raw writer requires the existing signed RSA update verification policy"
#endif
#include <cstring>
#include <string_view>

namespace tk::ota {
namespace {
constexpr std::size_t kPrefixBytes = sizeof(esp_image_header_t) +
    sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
static_assert(kPrefixBytes <= 512);
Effect error(esp_err_t result) noexcept {
    return result == ESP_OK ? Effect::Ok : result == ESP_ERR_NO_MEM ? Effect::NoMemory : Effect::Failed;
}
}
Effects RawWriter::effects() noexcept {
    return {this,kPrefixBytes,prefix,begin,write,end,select,abort,quiescent,release};
}
Effect RawWriter::prefix(void* p,const std::uint8_t* data,std::size_t n) noexcept {
    auto& s = self(p);
    if (!data || n != kPrefixBytes || s.handle_ || s.uncertain_) return Effect::Failed;
    esp_image_header_t image{};
    esp_app_desc_t description{};
    std::memcpy(&image,data,sizeof(image));
    std::memcpy(&description,data+sizeof(image)+sizeof(esp_image_segment_header_t),sizeof(description));
    const esp_app_desc_t* spi_description = nullptr;
#if defined(CONFIG_ESP_HTTPS_OTA_VERIFY_SPI_MODE) && CONFIG_ESP_HTTPS_OTA_VERIFY_SPI_MODE
    spi_description = &description;
#endif
    if (image.magic != ESP_IMAGE_HEADER_MAGIC || description.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        esp_ota_check_image_validity(ESP_PARTITION_TYPE_APP,&image,spi_description) != ESP_OK)
        return Effect::Failed;
    const auto available = tk::bounded_c_string_view(description.version);
    const auto expected = tk::bounded_c_string_view(s.policy_.expected);
    const auto current = tk::bounded_c_string_view(s.policy_.current);
    if (!tk::canonical_ota_version(available) || !tk::canonical_ota_version(expected) ||
        !tk::canonical_ota_version(current) || available != expected ||
        !tk::is_ota_update_available(available,current,s.policy_.target_pr,s.policy_.channel))
        return Effect::Failed;
    s.partition_ = esp_ota_get_next_update_partition(nullptr);
    const auto* running = esp_ota_get_running_partition();
    if (!s.partition_ || s.partition_ == running || s.partition_->type != ESP_PARTITION_TYPE_APP ||
        (s.partition_->address != 0x20000 && s.partition_->address != 0x210000) ||
        s.partition_->size != 0x1f0000) return Effect::Failed;
    return Effect::Ok;
}
Effect RawWriter::begin(void* p) noexcept {
    auto& s = self(p);
    if (!s.partition_ || s.handle_ || s.uncertain_) return Effect::Failed;
    // At this pin a failing begin may already publish a registered handle. Retain it for abort.
    const auto result = esp_ota_begin(s.partition_,OTA_WITH_SEQUENTIAL_WRITES,&s.handle_);
    if (result == ESP_OK && !s.handle_) { s.uncertain_ = true; return Effect::Uncertain; }
    return error(result);
}
Effect RawWriter::write(void* p,const std::uint8_t* data,std::size_t n) noexcept {
    auto& s = self(p);
    if (!s.handle_ || s.uncertain_ || !data || !n || n > kBlockBytes) return Effect::Failed;
    return error(esp_ota_write(s.handle_,data,n));
}
Effect RawWriter::end(void* p) noexcept {
    auto& s = self(p);
    if (!s.handle_ || s.uncertain_) return Effect::Failed;
    const auto h = s.handle_;
    s.handle_ = 0; // SDK consumes the registered handle even on verification failure.
    const auto result = esp_ota_end(h);
    s.validated_ = result == ESP_OK;
    return error(result); // Complete signature verification remains SDK-config/pin dependent.
}
Effect RawWriter::select(void* p) noexcept {
    auto& s = self(p);
    if (!s.validated_ || s.handle_ || s.uncertain_ || !s.partition_) return Effect::Failed;
    const auto result = esp_ota_set_boot_partition(s.partition_); // Keep validating public API.
    if (result == ESP_OK) return Effect::Committed;
    if (result == ESP_ERR_OTA_VALIDATE_FAILED) return Effect::Failed; // Before selector writes.
    s.uncertain_ = true; // Other returns may follow partial durable selector effects.
    return Effect::Uncertain;
}
bool RawWriter::abort(void* p) noexcept {
    auto& s = self(p);
    if (s.uncertain_) return false;
    if (!s.handle_) return true;
    const auto h = s.handle_;
    const auto result = esp_ota_abort(h);
    if (result != ESP_OK && result != ESP_ERR_NOT_FOUND) { s.uncertain_ = true; return false; }
    s.handle_ = 0;
    return true;
}
bool RawWriter::quiescent(void* p) noexcept {
    const auto& s = self(p);
    return !s.handle_ && !s.uncertain_;
}
bool RawWriter::release(void* p) noexcept {
    auto& s = self(p);
    return s.release_ && s.release_(s.owner_);
}
} // namespace tk::ota
