#include "sdkconfig.h"
#if defined(CONFIG_TESLA_CONFIG_REPAIR_USB) && CONFIG_TESLA_CONFIG_REPAIR_USB
#include "config_repair.hpp"
#include <algorithm>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#if !defined(CONFIG_ESP_CONSOLE_NONE) || !CONFIG_ESP_CONSOLE_NONE
#error "Maintenance owns raw I/O; SDK stdio console must be disabled"
#endif
#if (defined(CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG) && CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG) || \
    (defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED) && CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED)
#error "Maintenance raw USB owner excludes SDK primary and secondary USB stdio"
#endif
#if !defined(CONFIG_LOG_MAXIMUM_LEVEL) || CONFIG_LOG_MAXIMUM_LEVEL != 0
#error "Maintenance raw I/O requires compile-time SDK logging disabled"
#endif
#if CONFIG_IDF_TARGET_ESP32
#include <driver/uart.h>
#include <hal/uart_ll.h>
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6
#include <driver/usb_serial_jtag.h>
#if !defined(CONFIG_ESP_CONSOLE_SECONDARY_NONE) || !CONFIG_ESP_CONSOLE_SECONDARY_NONE
#error "Maintenance native USB requires no secondary SDK console"
#endif
#if !defined(CONFIG_USJ_ENABLE_USB_SERIAL_JTAG) || !CONFIG_USJ_ENABLE_USB_SERIAL_JTAG
#error "Maintenance native USB-Serial-JTAG route must be enabled"
#endif
#else
#error "Unsupported maintenance target"
#endif

namespace tk::repair {
namespace {
bool expired(std::uint64_t until) noexcept {
    const auto now=esp_timer_get_time();
    return now<0 || static_cast<std::uint64_t>(now)>=until;
}
void yield_io() noexcept { vTaskDelay(1); }
[[noreturn]] void console_park() noexcept { for(;;)vTaskDelay(pdMS_TO_TICKS(10000)); }

// Only this early maintenance app uses the driver. SDK primary and secondary consoles are disabled, compile-time logging
// is off, no application task/service has started, and an existing driver is refused. UART TX
// never uses uart_tx_chars()/uart_write_bytes(): pinned IDF's tx_mux can wait portMAX_DELAY.
class RawConsole final:public Console {
public:
    bool init() noexcept {
#if CONFIG_IDF_TARGET_ESP32
        if(uart_is_driver_installed(UART_NUM_0))return false;
        uart_config_t config{};
        config.baud_rate=115200;config.data_bits=UART_DATA_8_BITS;
        config.parity=UART_PARITY_DISABLE;config.stop_bits=UART_STOP_BITS_1;
        config.flow_ctrl=UART_HW_FLOWCTRL_DISABLE;config.source_clk=UART_SCLK_REF_TICK;
        if(uart_param_config(UART_NUM_0,&config)!=ESP_OK)return false;
        // The admitted ordinary bootloader owns UART0 routing. Do not select GPIOs from a
        // board guess or reroute pins: preservation of that real board route is a USB gate.
        if(uart_set_pin(UART_NUM_0,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE,
                        UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE)!=ESP_OK)return false;
        if(uart_driver_install(UART_NUM_0,1024,0,0,nullptr,0)!=ESP_OK)return false;
#else
        if(usb_serial_jtag_is_driver_installed())return false;
        usb_serial_jtag_driver_config_t config{};config.tx_buffer_size=256;config.rx_buffer_size=1024;
        if(usb_serial_jtag_driver_install(&config)!=ESP_OK)return false;
#endif
        ready_=true;return true;
    }
    bool read_exact(std::uint8_t* dst,std::size_t size,std::uint64_t until) noexcept override {
        if(!ready_ || (!dst&&size))return false;
        std::size_t done=0;
        while(done<size) {
            if(expired(until))return false;
            const auto chunk=std::min<std::size_t>(64,size-done);
#if CONFIG_IDF_TARGET_ESP32
            const int count=uart_read_bytes(UART_NUM_0,dst+done,chunk,0);
#else
            const int count=usb_serial_jtag_read_bytes(dst+done,chunk,0);
#endif
            if(count<0 || static_cast<std::size_t>(count)>chunk)return false;
            done+=static_cast<std::size_t>(count);
            if(count==0)yield_io();
        }
        return !expired(until);
    }
    bool write_exact(const std::uint8_t* src,std::size_t size,std::uint64_t until) noexcept override {
        if(!ready_ || (!src&&size))return false;
        std::size_t done=0;
        while(done<size) {
            if(expired(until))return false;
            auto chunk=std::min<std::size_t>(64,size-done);
#if CONFIG_IDF_TARGET_ESP32
            // uart_ll_get_txfifo_len returns free FIFO space, not occupied bytes. There is
            // no competing producer or TX ring; the ISR handles RX only (TX buffer size0).
            chunk=std::min<std::size_t>(chunk,uart_ll_get_txfifo_len(UART_LL_GET_HW(UART_NUM_0)));
            uart_ll_write_txfifo(UART_LL_GET_HW(UART_NUM_0),src+done,chunk);
            const auto count=chunk;
#else
            const int written=usb_serial_jtag_write_bytes(src+done,chunk,0);
            if(written<0 || static_cast<std::size_t>(written)>chunk)return false;
            const auto count=static_cast<std::size_t>(written);
#endif
            done+=count;
            if(count==0)yield_io();
        }
        for(;;) {
            if(expired(until))return false;
#if CONFIG_IDF_TARGET_ESP32
            if(uart_ll_is_tx_idle(UART_LL_GET_HW(UART_NUM_0)))return true;
#else
            if(usb_serial_jtag_wait_tx_done(0)==ESP_OK)return true;
#endif
            yield_io();
        }
    }
private:
    bool ready_=false;
};
}
[[noreturn]] void console_entry() noexcept {
    // No SDK log/error text, console echo, automatic retry, normal startup or reboot fallback.
    // Driver installation may allocate; a failure parks before any INFO/APPLY/NVS write.
    RawConsole io;
    if(!io.init())console_park();
    run(io);
}
} // namespace tk::repair
#endif
