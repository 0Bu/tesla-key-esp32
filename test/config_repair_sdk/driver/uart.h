#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stddef.h>
typedef int uart_port_t;
struct QueueDefinition;typedef QueueDefinition* QueueHandle_t;
typedef struct {int baud_rate;int data_bits;int parity;int stop_bits;int flow_ctrl;int source_clk;} uart_config_t;
#define UART_NUM_0 0
#define UART_DATA_8_BITS 3
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_REF_TICK 1
#define UART_PIN_NO_CHANGE -1
extern "C" {
bool uart_is_driver_installed(uart_port_t uart_num);
esp_err_t uart_param_config(uart_port_t uart_num,const uart_config_t *uart_config);
esp_err_t _uart_set_pin6(uart_port_t uart_num,int tx_io_num,int rx_io_num,int rts_io_num,int cts_io_num,int dtr_io_num,int dsr_io_num);
esp_err_t uart_driver_install(uart_port_t uart_num,int rx_buffer_size,int tx_buffer_size,int queue_size,QueueHandle_t *uart_queue,int intr_alloc_flags);
int uart_read_bytes(uart_port_t uart_num,void *buf,uint32_t length,TickType_t ticks_to_wait);
}

static inline esp_err_t _uart_set_pin4(uart_port_t p,int tx,int rx,int rts,int cts){return _uart_set_pin6(p,tx,rx,rts,cts,-1,-1);}
#define uart_set_pin _uart_set_pin4
