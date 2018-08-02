/*
 * Copyright (c) 2018 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "nrf.h"
#include "nrf_drv_usbd.h"
#include "nrf_drv_clock.h"
#include "nrf_gpio.h"
#include "nrf_delay.h"
#include "nrf_drv_power.h"

#include "app_error.h"
#include "app_util.h"
#include "app_usbd_core.h"
#include "app_usbd.h"
#include "app_usbd_string_desc.h"
#include "app_usbd_cdc_acm.h"
#include "app_usbd_serial_num.h"
#include "app_fifo.h"
#include "usb_hal_cdc.h"
#include "logging.h"

/**
 * @brief Enable power USB detection
 *
 * Configure if example supports USB port connection
 */
#ifndef USBD_POWER_DETECTION
#define USBD_POWER_DETECTION true
#endif

#define CDC_ACM_COMM_INTERFACE  0
#define CDC_ACM_COMM_EPIN       NRF_DRV_USBD_EPIN2

#define CDC_ACM_DATA_INTERFACE  1
#define CDC_ACM_DATA_EPIN       NRF_DRV_USBD_EPIN1
#define CDC_ACM_DATA_EPOUT      NRF_DRV_USBD_EPOUT1

#define SERIAL_NUMBER_STRING_SIZE (12) 
//uint16_t APP_USBD_STRING_SERIAL[SERIAL_NUMBER_STRING_SIZE + 1];

typedef enum {
     USB_MODE_NONE,
     USB_MODE_CDC_UART,
     USB_MODE_HID
} usb_mode_t;

typedef enum {
    POWER_STATE_REMOVED,
    POWER_STATE_DETECTED,
    POWER_STATE_READY
} power_state_t; 

typedef struct {
    bool                    initialized;
    volatile power_state_t  power_state;
    usb_mode_t              mode;

    app_fifo_t              rx_fifo;
    app_fifo_t              tx_fifo;

    volatile bool           com_opened;
    volatile bool           transmitting;
    volatile bool           rx_ovf;

    // USB CDC 
    uint32_t                baudrate;       // just for compatibility, no need baudrate configuration on nRF52
} usb_instance_t;

static usb_instance_t m_usb_instance = {0};

#define READ_SIZE 1
#define SEND_SIZE NRF_DRV_USBD_EPSIZE   
static char         m_rx_buffer[READ_SIZE];  
static char         m_tx_buffer[SEND_SIZE];

static void cdc_acm_user_ev_handler(app_usbd_class_inst_t const * p_inst,
                                    app_usbd_cdc_acm_user_event_t event);
/**
 * @brief CDC_ACM class instance
 * */
APP_USBD_CDC_ACM_GLOBAL_DEF(m_app_cdc_acm,
                            cdc_acm_user_ev_handler,
                            CDC_ACM_COMM_INTERFACE,
                            CDC_ACM_DATA_INTERFACE,
                            CDC_ACM_COMM_EPIN,
                            CDC_ACM_DATA_EPIN,
                            CDC_ACM_DATA_EPOUT,
                            APP_USBD_CDC_COMM_PROTOCOL_AT_V250
);

#define FIFO_LENGTH(p_fifo)     fifo_length(p_fifo)  /**< Macro for calculating the FIFO length. */
#define IS_FIFO_FULL(p_fifo)    fifo_full(p_fifo)
static __INLINE uint32_t fifo_length(app_fifo_t * p_fifo) {
    uint32_t tmp = p_fifo->read_pos;
    return p_fifo->write_pos - tmp;
}
static __INLINE bool fifo_full(app_fifo_t * p_fifo) {
    return (FIFO_LENGTH(p_fifo) > p_fifo->buf_size_mask);
}

/**
 * @brief User event handler @ref app_usbd_cdc_acm_user_ev_handler_t (headphones)
 * */
static void cdc_acm_user_ev_handler(app_usbd_class_inst_t const * p_inst,
                                    app_usbd_cdc_acm_user_event_t event)
{
    static bool io_pending = false;

    switch (event) {
        case APP_USBD_CDC_ACM_USER_EVT_PORT_OPEN: {
            // reset buffer state
            m_usb_instance.com_opened = true;
            m_usb_instance.rx_ovf = false;
            app_fifo_flush(&m_usb_instance.tx_fifo);
            app_fifo_flush(&m_usb_instance.rx_fifo);

            ret_code_t ret;
            /*Setup first transfer*/
            ret = app_usbd_cdc_acm_read(&m_app_cdc_acm, m_rx_buffer, READ_SIZE);
            if (ret == NRF_ERROR_IO_PENDING)
            {
                io_pending = true;
            }
            LOG_DEBUG(TRACE, "com open!");
            break;
        }
        case APP_USBD_CDC_ACM_USER_EVT_PORT_CLOSE:{
            m_usb_instance.com_opened = false;
            LOG_DEBUG(TRACE, "com close!!! %d", FIFO_LENGTH(&m_usb_instance.tx_fifo));
            break;
        }
        case APP_USBD_CDC_ACM_USER_EVT_TX_DONE: {
            uint8_t data = 0;
            uint16_t size = 0;

            if (m_usb_instance.com_opened == false)
            {
                m_usb_instance.transmitting = false;
                LOG_DEBUG(TRACE, "tx done, but com close!!! %d", FIFO_LENGTH(&m_usb_instance.tx_fifo));
                
                return;
            }

            while ((size < SEND_SIZE) && (app_fifo_get(&m_usb_instance.tx_fifo, &data) == NRF_SUCCESS))
            {
                m_tx_buffer[size++] = data;
            }

            if (size == 0)
            {
                m_usb_instance.transmitting = false;
            }
            else
            {
                m_usb_instance.transmitting = true;  // app_usbd_cdc_acm_write() cause interrupt before return!
                uint32_t ret = app_usbd_cdc_acm_write(&m_app_cdc_acm, m_tx_buffer, size);
                if (ret != NRF_SUCCESS)
                {
                    m_usb_instance.transmitting = false;
                    LOG_DEBUG(ERROR, "ERROR: send data FAILED!");
                }
            }
            break;
        }
        case APP_USBD_CDC_ACM_USER_EVT_RX_DONE: {
            ret_code_t ret;
            do
            {
                if (io_pending)
                {
                    if (app_fifo_put(&m_usb_instance.rx_fifo, m_rx_buffer[0]))
                    {
                        m_usb_instance.rx_ovf = true;
                        LOG_DEBUG(TRACE, "ERROR!!! rx overflow!!!");
                    }
                    io_pending = false;
                }

                /* Fetch data until internal buffer is empty */
                ret = app_usbd_cdc_acm_read(&m_app_cdc_acm,
                                            m_rx_buffer,
                                            READ_SIZE);
                if (ret == NRF_SUCCESS)    
                {
                    if (app_fifo_put(&m_usb_instance.rx_fifo, m_rx_buffer[0]))
                    {
                        m_usb_instance.rx_ovf = true;
                        LOG_DEBUG(TRACE, "ERROR!!! rx overflow!!!");
                    }
                }  
                else if (ret == NRF_ERROR_IO_PENDING)
                {
                    io_pending = true;
                }
            } while (ret == NRF_SUCCESS);
            break;
        }
        default:
            break;
    }
}

static void usbd_user_ev_handler(app_usbd_event_type_t event)
{
    switch (event) {
        case APP_USBD_EVT_DRV_SUSPEND: {
            LOG_DEBUG(TRACE, "APP_USBD_EVT_DRV_SUSPEND");
            break;
        }
        case APP_USBD_EVT_DRV_RESUME: {
            LOG_DEBUG(TRACE, "APP_USBD_EVT_DRV_RESUME");
            break;
        }
        case APP_USBD_EVT_STARTED: {
            // triggered by app_usbd_start()
            break;
        }
        case APP_USBD_EVT_STOPPED: {
            // triggered by app_usbd_stop()
            app_usbd_disable();
            break;
        }
        case APP_USBD_EVT_POWER_DETECTED: {
            m_usb_instance.power_state = POWER_STATE_DETECTED;
            if (!nrf_drv_usbd_is_enabled())
            {
                app_usbd_enable();
            }
            break;
        }
        case APP_USBD_EVT_POWER_REMOVED: {
            app_usbd_stop(); 
            m_usb_instance.power_state = POWER_STATE_REMOVED;
            break;
        }
        case APP_USBD_EVT_POWER_READY: {
            m_usb_instance.power_state = POWER_STATE_READY;
            app_usbd_start();
            m_usb_instance.rx_ovf = 0;
            m_usb_instance.com_opened = false;
            break;
        }
        default: 
            break;
    }
}

int usb_hal_init(void) {
    if (m_usb_instance.initialized) {   
        return 0;
    }

    ret_code_t ret;
    static const app_usbd_config_t usbd_config = {
        .ev_state_proc = usbd_user_ev_handler
    };

    ret = nrf_drv_clock_init();
    if (ret != NRF_ERROR_MODULE_ALREADY_INITIALIZED) {
        SPARK_ASSERT(ret == NRF_SUCCESS);
    }

    nrf_drv_clock_lfclk_request(NULL);
    while(!nrf_drv_clock_lfclk_is_running()) {
        /* Just waiting */
    }

    app_usbd_serial_num_generate();

    ret = app_usbd_init(&usbd_config);
    SPARK_ASSERT(ret == NRF_SUCCESS);

    m_usb_instance.initialized = true;  

    return 0;
}

int usb_uart_init(uint8_t *rx_buf, uint16_t rx_buf_size, uint8_t *tx_buf, uint16_t tx_buf_size) {
    uint32_t ret;

    if (m_usb_instance.mode == USB_MODE_CDC_UART) {
        return 0;
    }

    if (app_fifo_init(&m_usb_instance.rx_fifo, rx_buf, rx_buf_size)) {
        return  -1;
    }

    if (app_fifo_init(&m_usb_instance.tx_fifo, tx_buf, tx_buf_size)) {
        return  -2;
    }

    app_usbd_class_inst_t const * class_cdc_acm = app_usbd_cdc_acm_class_inst_get(&m_app_cdc_acm);
    ret = app_usbd_class_append(class_cdc_acm);
    SPARK_ASSERT(ret == NRF_SUCCESS);

    if (USBD_POWER_DETECTION) {
        ret = app_usbd_power_events_enable();
        SPARK_ASSERT(ret == NRF_SUCCESS);
    } else {
        LOG_DEBUG(TRACE, "No USB power detection enabled\r\nStarting USB now");

        app_usbd_enable();
        app_usbd_start();
    }

    m_usb_instance.mode = USB_MODE_CDC_UART;

    return 0;
}

int usb_uart_send(uint8_t data[], uint16_t size) {
    if (!m_usb_instance.com_opened) {
        return 0;
    }

    if ((__get_PRIMASK() & 1)) {
        return 0;
    }

    for (int i = 0; i < size; i++) {
        // wait until tx fifo is available
        while (IS_FIFO_FULL(&m_usb_instance.tx_fifo));
        app_fifo_put(&m_usb_instance.tx_fifo, data[i]);
    }

    uint32_t ret;
    uint16_t pre_send_size = 0;
    uint8_t  pre_send_data = 0;

    // trigger first transmitting
    if (!m_usb_instance.transmitting) {
        while ((pre_send_size < SEND_SIZE) && 
               (app_fifo_get(&m_usb_instance.tx_fifo, &pre_send_data) == NRF_SUCCESS))
        {
            m_tx_buffer[pre_send_size++] = pre_send_data;
        }

        m_usb_instance.transmitting = true; // app_usbd_cdc_acm_write() cause interrupt before return!
        ret = app_usbd_cdc_acm_write(&m_app_cdc_acm, m_tx_buffer, pre_send_size);
        SPARK_ASSERT(ret == NRF_SUCCESS); 
        if (ret != NRF_SUCCESS) {
            m_usb_instance.transmitting = false;
            pre_send_size = 0;
            LOG_DEBUG(ERROR, "ERROR: send data FAILED!");
        }
    }

    return pre_send_size;
}

void usb_uart_set_baudrate(uint32_t baudrate) {
    m_usb_instance.baudrate = baudrate;
}

uint32_t usb_uart_get_baudrate(void) {
    return m_usb_instance.baudrate;
}

void usb_hal_attach(void) {
    if ((m_usb_instance.power_state == POWER_STATE_REMOVED) ||
        (m_usb_instance.power_state == POWER_STATE_READY))
    {
        return;
    }

    if (!nrf_drv_usbd_is_enabled()) {
        app_usbd_enable();
    }
    app_usbd_start();

    m_usb_instance.rx_ovf = 0;
    m_usb_instance.com_opened = false;
}

void usb_hal_detach(void) {
    if (m_usb_instance.power_state == POWER_STATE_REMOVED) {
        return;
    }

    app_usbd_stop();
    if (nrf_drv_usbd_is_enabled()) {
        app_usbd_disable();
    }
}

int usb_uart_available_rx_data(void) {
    return FIFO_LENGTH(&m_usb_instance.rx_fifo); 
}

uint8_t usb_uart_get_rx_data(void) {
    uint8_t data = 0;
    if (app_fifo_get(&m_usb_instance.rx_fifo, &data)) {
        return 0;
    }
    return data;
}

uint8_t usb_uart_peek_rx_data(uint8_t index) {
    uint8_t data = 0;
    if (app_fifo_peek(&m_usb_instance.rx_fifo, index, &data)) {
        return 0;
    }
    return data;
}

void usb_uart_flush_rx_data(void) {
    app_fifo_flush(&m_usb_instance.rx_fifo);
}

void usb_uart_flush_tx_data(void) {
    app_fifo_flush(&m_usb_instance.tx_fifo);
}

int usb_uart_available_tx_data(void) {
    return FIFO_LENGTH(&m_usb_instance.tx_fifo); 
}

bool usb_hal_is_enabled(void) {
    return m_usb_instance.initialized;
}

bool usb_hal_is_connected(void) {
    return m_usb_instance.com_opened;
}
