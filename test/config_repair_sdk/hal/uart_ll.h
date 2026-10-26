#pragma once
#include <stdint.h>
struct uart_dev_t {};
extern uart_dev_t repair_fixture_uart;
#define UART_LL_GET_HW(number) (&repair_fixture_uart)
extern "C" {
uint32_t uart_ll_get_txfifo_len(uart_dev_t *hw);
void uart_ll_write_txfifo(uart_dev_t *hw,const uint8_t *buf,uint32_t length);
bool uart_ll_is_tx_idle(uart_dev_t *hw);
}
