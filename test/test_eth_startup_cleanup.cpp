// Actual Ethernet cleanup/callers with passive SDK doubles. No SDK ABI or scheduler claim.
// Reviewed source: ESP-IDF v6.1 esp_eth.c/esp_eth_netif_glue.c and W5500 commit
// 1f19456ec90b60424583a4138c075a78e1ab2297, admitted by the adapter generator.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { std::fprintf(stderr, "Ethernet CHECK %d: %s\n", __LINE__, #expr); std::exit(42); } } while (0)

using esp_err_t = int;
static constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 0x103;
struct FixtureEvents {};
struct esp_netif_t {};
struct FixtureDriver {};
struct FixtureGlue {};
using EventGroupHandle_t = FixtureEvents*;
using esp_eth_handle_t = FixtureDriver*;
using esp_eth_netif_glue_handle_t = FixtureGlue*;
struct esp_eth_mac_t { esp_err_t (*del)(esp_eth_mac_t*); };
struct esp_eth_phy_t { esp_err_t (*del)(esp_eth_phy_t*); };
static esp_err_t delete_mac(esp_eth_mac_t*);
static esp_err_t delete_phy(esp_eth_phy_t*);

enum Resource : unsigned { Events = 1, Netif = 2, Mac = 4, Phy = 8, Driver = 16,
                           Glue = 32, EthHandler = 64, IpHandler = 128, All = 255 };
struct Model {
    FixtureEvents events;
    esp_netif_t netif;
    FixtureDriver driver;
    FixtureGlue glue;
    esp_eth_mac_t mac{delete_mac};
    esp_eth_phy_t phy{delete_phy};
    bool events_alive{}, netif_alive{}, mac_alive{}, phy_alive{}, driver_alive{}, glue_alive{};
    bool eth_registered{}, ip_registered{}, spi_up{true}, activated{}, pending_rx{}, withdrawn{};
    esp_err_t stop_result{ESP_OK}, fault_result{ESP_FAIL};
    std::string fault;
    std::vector<std::string> calls;
};
static Model model;
static esp_eth_handle_t s_eth_handle;
static esp_eth_netif_glue_handle_t s_eth_glue;
static esp_netif_t* s_eth_netif;
static EventGroupHandle_t s_eth_events;
static std::atomic<bool> s_eth_link{true};
enum class NetLink { Eth };
static constexpr int IP_EVENT = 1, IP_EVENT_ETH_GOT_IP = 2, ETH_EVENT = 3, ESP_EVENT_ANY_ID = -1;
static void eth_event_handler(void*, int, int, void*) {}

static bool step(const char* name, bool quieting_only = false) {
    model.calls.emplace_back(name);
    // Even a successful stop is NOT modeled as a join. Pending RX still owns the full graph.
    CHECK(!model.activated || quieting_only);
    if (model.pending_rx) {
        CHECK(model.netif_alive && model.mac_alive && model.events_alive && model.spi_up);
    }
    return model.fault == name;
}
static esp_err_t esp_eth_stop(esp_eth_handle_t handle) {
    CHECK(handle == &model.driver && model.driver_alive);
    (void)step("stop", true);
    return model.stop_result;
}
static esp_err_t esp_event_handler_unregister(int base, int id, decltype(eth_event_handler)* handler) {
    CHECK(handler == eth_event_handler);
    const bool ip = base == IP_EVENT;
    CHECK(ip ? id == IP_EVENT_ETH_GOT_IP : base == ETH_EVENT && id == ESP_EVENT_ANY_ID);
    CHECK(ip ? model.ip_registered : model.eth_registered);
    if (step(ip ? "ip_unregister" : "eth_unregister")) return model.fault_result;
    (ip ? model.ip_registered : model.eth_registered) = false;
    return ESP_OK;
}
static void link_down(NetLink link) {
    CHECK(link == NetLink::Eth && !model.ip_registered && !model.eth_registered);
    (void)step("withdraw_globals");
    model.withdrawn = true;
}
static esp_err_t esp_eth_del_netif_glue(esp_eth_netif_glue_handle_t glue) {
    CHECK(glue == &model.glue && model.glue_alive);
    CHECK(!model.ip_registered && !model.eth_registered);
    if (step("delete_glue")) return model.fault_result;
    model.glue_alive = false;
    // The SDK input binding still names netif; glue deletion does not detach that pointer.
    return ESP_OK;
}
static esp_err_t esp_eth_driver_uninstall(esp_eth_handle_t handle) {
    CHECK(handle == &model.driver && model.driver_alive && !model.glue_alive);
    CHECK(model.netif_alive && model.events_alive && model.spi_up);
    if (step("uninstall_driver")) return model.fault_result;
    model.driver_alive = false;
    return ESP_OK;
}
static esp_err_t delete_mac(esp_eth_mac_t* mac) {
    CHECK(mac == &model.mac && model.mac_alive && !model.driver_alive && model.spi_up);
    if (step("delete_mac")) return model.fault_result;
    // This path has never activated RX. No cross-core task-join guarantee is assumed.
    CHECK(!model.activated && !model.pending_rx);
    model.mac_alive = false;
    return ESP_OK;
}
static esp_err_t delete_phy(esp_eth_phy_t* phy) {
    CHECK(phy == &model.phy && model.phy_alive && !model.driver_alive && !model.mac_alive);
    if (step("delete_phy")) return model.fault_result;
    model.phy_alive = false;
    return ESP_OK;
}
static void esp_netif_destroy(esp_netif_t* netif) {
    CHECK(netif == &model.netif && model.netif_alive);
    CHECK(!model.glue_alive && !model.driver_alive && !model.mac_alive && !model.phy_alive);
    CHECK(!model.ip_registered && !model.eth_registered);
    (void)step("destroy_netif");
    model.netif_alive = false;
}
static void vEventGroupDelete(EventGroupHandle_t events) {
    CHECK(events == &model.events && model.events_alive);
    CHECK(!model.netif_alive && !model.driver_alive && !model.mac_alive && !model.phy_alive);
    CHECK(!model.ip_registered && !model.eth_registered);
    (void)step("delete_events");
    model.events_alive = false;
}
static bool eth_release_spi_bus() {
    CHECK(!model.events_alive && !model.netif_alive && !model.driver_alive);
    CHECK(!model.mac_alive && !model.phy_alive && !model.glue_alive);
    if (step("free_spi")) return false;
    model.spi_up = false;
    return true;
}
struct Fatal { const char* component; };
[[noreturn]] static void boot_fatal(const char* component) { throw Fatal{component}; }
#define ESP_LOGE(...) ((void)0)
#include "eth_startup_adapter.generated.hpp"
#undef ESP_LOGE

static EthStartupResources acquire(unsigned mask, bool published = false, bool activated = false) {
    model = Model{};
    model.events_alive = (mask & Events) != 0;
    model.netif_alive = (mask & Netif) != 0;
    model.mac_alive = (mask & Mac) != 0;
    model.phy_alive = (mask & Phy) != 0;
    model.driver_alive = (mask & Driver) != 0;
    model.glue_alive = (mask & Glue) != 0;
    model.eth_registered = (mask & EthHandler) != 0;
    model.ip_registered = (mask & IpHandler) != 0;
    model.activated = activated;
    model.pending_rx = activated;
    EthStartupResources r;
    r.events = model.events_alive ? &model.events : nullptr;
    r.netif = model.netif_alive ? &model.netif : nullptr;
    r.mac = model.mac_alive ? &model.mac : nullptr;
    r.phy = model.phy_alive ? &model.phy : nullptr;
    r.handle = model.driver_alive ? &model.driver : nullptr;
    r.glue = model.glue_alive ? &model.glue : nullptr;
    r.eth_handler_registered = model.eth_registered;
    r.ip_handler_registered = model.ip_registered;
    r.published = published;
    r.start_attempted = activated;
    s_eth_events = published ? r.events : nullptr;
    s_eth_netif = published ? r.netif : nullptr;
    s_eth_handle = published ? r.handle : nullptr;
    s_eth_glue = published ? r.glue : nullptr;
    s_eth_link.store(true);
    return r;
}
static void check_owner_matches_model(const EthStartupResources& r) {
    CHECK((r.events != nullptr) == model.events_alive);
    CHECK((r.netif != nullptr) == model.netif_alive);
    CHECK((r.mac != nullptr) == model.mac_alive);
    CHECK((r.phy != nullptr) == model.phy_alive);
    CHECK((r.handle != nullptr) == model.driver_alive);
    CHECK((r.glue != nullptr) == model.glue_alive);
    CHECK(r.ip_handler_registered == model.ip_registered);
    CHECK(r.eth_handler_registered == model.eth_registered);
}
static void check_retained(const EthStartupResources& r, const EthStartupResources& before) {
    CHECK(r.events == before.events && r.netif == before.netif && r.handle == before.handle);
    CHECK(r.glue == before.glue && r.mac == before.mac && r.phy == before.phy);
    CHECK(r.ip_handler_registered == before.ip_handler_registered);
    CHECK(r.eth_handler_registered == before.eth_handler_registered);
    CHECK(r.published == before.published && r.start_attempted == before.start_attempted);
    CHECK(s_eth_events == (before.published ? before.events : nullptr));
    CHECK(s_eth_netif == (before.published ? before.netif : nullptr));
    CHECK(s_eth_handle == (before.published ? before.handle : nullptr));
    CHECK(s_eth_glue == (before.published ? before.glue : nullptr));
    CHECK(s_eth_link.load() && !model.withdrawn && model.spi_up && model.pending_rx);
    check_owner_matches_model(r);
}
static void activated_graph_is_retained() {
    for (const auto result : {ESP_OK, ESP_ERR_INVALID_STATE, ESP_FAIL, 0x7fff}) {
        for (const bool published : {false, true}) {
            for (const bool handle_present : {false, true}) {
                auto r = acquire(handle_present ? unsigned(All) : unsigned(All & ~Driver), published, true);
                model.stop_result = result;
                const auto before = r;
                CHECK(!eth_cleanup_startup(r));
                CHECK(model.calls == (handle_present ? std::vector<std::string>{"stop"}
                                                    : std::vector<std::string>{}));
                check_retained(r, before);
            }
        }
    }
}
static void passive_acquisition_prefixes_unwind() {
    for (const unsigned mask : {0u, unsigned(Events), unsigned(Events | Netif),
             unsigned(Events | Netif | Mac), unsigned(Events | Netif | Phy),
             unsigned(Events | Netif | Mac | Phy), unsigned(Events | Netif | Mac | Phy | Driver),
             unsigned(Events | Netif | Mac | Phy | Driver | Glue), unsigned(All & ~IpHandler), unsigned(All)}) {
        auto r = acquire(mask);
        CHECK(eth_cleanup_startup(r));
        check_owner_matches_model(r);
        CHECK(!r.events && !r.netif && !r.mac && !r.phy && !r.handle && !r.glue);
        CHECK(!r.ip_handler_registered && !r.eth_handler_registered && !model.spi_up);
    }
    auto r = acquire(All, true);
    CHECK(eth_cleanup_startup(r));
    const std::vector<std::string> expected{"ip_unregister", "eth_unregister", "withdraw_globals",
        "delete_glue", "uninstall_driver", "delete_mac", "delete_phy", "destroy_netif",
        "delete_events", "free_spi"};
    CHECK(model.calls == expected);
    CHECK(!r.published && !s_eth_handle && !s_eth_glue && !s_eth_netif && !s_eth_events);
    CHECK(!s_eth_link.load());
}
static void passive_failures_retain_the_dependent_tail() {
    const std::vector<std::string> stages{"ip_unregister", "eth_unregister", "withdraw_globals",
        "delete_glue", "uninstall_driver", "delete_mac", "delete_phy", "destroy_netif",
        "delete_events", "free_spi"};
    for (const char* failure : {"ip_unregister", "eth_unregister", "delete_glue", "uninstall_driver",
                                "delete_mac", "delete_phy", "free_spi"}) {
        for (const auto result : {ESP_FAIL, ESP_ERR_INVALID_STATE}) {
            auto r = acquire(All, true);
            model.fault = failure;
            model.fault_result = result;
            CHECK(!eth_cleanup_startup(r));
            std::vector<std::string> prefix;
            for (const auto& stage : stages) {
                prefix.push_back(stage);
                if (stage == failure) break;
            }
            CHECK(model.calls == prefix);
            check_owner_matches_model(r);
            CHECK(model.spi_up);
            if (model.fault != "free_spi") CHECK(r.netif && r.events);
            CHECK(r.published == !model.withdrawn);
            if (r.published) CHECK(s_eth_netif == r.netif && s_eth_events == r.events);
        }
    }
}
static void actual_callers_park_on_retention() {
    auto r = acquire(All, true, true);
    const auto before = r;
    bool parked = false;
    try { (void)eth_startup_fallback(r); }
    catch (const Fatal& fatal) { parked = true; CHECK(std::strcmp(fatal.component, "Ethernet startup cleanup") == 0); }
    CHECK(parked);
    check_retained(r, before);
    r = acquire(All, true, true);
    parked = false;
    try { eth_startup_fatal(r, "original startup failure"); }
    catch (const Fatal& fatal) { parked = true; CHECK(std::strcmp(fatal.component, "Ethernet startup cleanup") == 0); }
    CHECK(parked);
    r = acquire(All);
    parked = false;
    try { eth_startup_fatal(r, "original startup failure"); }
    catch (const Fatal& fatal) { parked = true; CHECK(std::strcmp(fatal.component, "original startup failure") == 0); }
    CHECK(parked && !model.spi_up);
    r = acquire(All);
    CHECK(!eth_startup_fallback(r) && !model.spi_up);
}
int main() {
    activated_graph_is_retained();
    passive_acquisition_prefixes_unwind();
    passive_failures_retain_the_dependent_tail();
    actual_callers_park_on_retention();
    std::printf("PASS %u actual Ethernet cleanup/caller checks\n", checks);
}
