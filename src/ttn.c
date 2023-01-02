/*******************************************************************************
 *
 * ttn-esp32 - The Things Network device library for ESP-IDF / SX127x
 *
 * Copyright (c) 2018-2021 Manuel Bleichenbacher
 *
 * Licensed under MIT License
 * https://opensource.org/licenses/MIT
 *
 * High-level C API for ttn-esp32.
 *******************************************************************************/

#include "ttn.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "hal/hal_esp32.h"
#include "lmic/lmic.h"
#include "ttn_logging.h"
#include "ttn_provisioning.h"
#include "Display.h"

#define TAG "ttn"

#define DEFAULT_MAX_TX_POWER -1000

void ResetSeconds(void);

typedef enum
{
    TTN_DISCONNECTED = 0,
    TTN_CONNECTED
} ttn_state_t;

/**
 * @brief Reason the user code is waiting
 */
typedef enum
{
    TTN_WAITING_NONE,
    TTN_WAITING_FOR_JOIN,
    TTN_WAITING_FOR_TRANSMISSION
} ttn_waiting_reason_t;

/**
 * @brief Event type
 */
typedef enum
{
    TTN_EVENT_NONE,
    TTN_EVNT_JOIN_COMPLETED,
    TTN_EVENT_JOIN_FAILED,
    TTN_EVENT_MESSAGE_RECEIVED,
    TTN_EVENT_TRANSMISSION_COMPLETED,
    TTN_EVENT_TRANSMISSION_FAILED
} ttn_event_t;

/**
 * @brief Event message sent from LMIC task to waiting client task
 */
typedef struct
{
    ttn_event_t event;
    uint8_t port;
    const uint8_t *message;
    size_t message_size;
} ttn_lmic_event_t;

static ttn_state_t ttnState = TTN_DISCONNECTED;

static bool is_started;
static QueueHandle_t lmic_event_queue;
static ttn_message_cb message_callback;
static ttn_waiting_reason_t waiting_reason = TTN_WAITING_NONE;
static ttn_rf_settings_t last_rf_settings[4];
static ttn_rx_tx_window_t current_rx_tx_window;
static int subband = 2;
static ttn_data_rate_t join_data_rate = TTN_DR_JOIN_DEFAULT;
static int max_tx_power = DEFAULT_MAX_TX_POWER;

static bool join_core(void);
static void config_rf_params(void);
static void event_callback(void *user_data, ev_t event);
static void message_received_callback(void *user_data, uint8_t port, const uint8_t *message, size_t message_size);
static void message_transmitted_callback(void *user_data, int success);
static void save_rf_settings(ttn_rf_settings_t *rf_settings);
static void clear_rf_settings(ttn_rf_settings_t *rf_settings);

void ttn_init(void)
{
#if defined(TTN_IS_DISABLED)
    ESP_LOGE(TAG, "TTN is disabled. Configure a frequency plan using 'make menuconfig'");
    ASSERT(0);
#endif

    message_callback = NULL;
    hal_esp32_init_critical_section();
}

void ttn_configure_pins(spi_host_device_t spi_host, uint8_t nss, uint8_t rxtx, uint8_t rst, uint8_t dio0, uint8_t dio1)
{
    hal_esp32_configure_pins(spi_host, nss, rxtx, rst, dio0, dio1);

#if LMIC_ENABLE_event_logging
    ttn_log_init();
#endif

    LMIC_registerEventCb(event_callback, NULL);
    LMIC_registerRxMessageCb(message_received_callback, NULL);

    os_init_ex(NULL);
    ttn_reset();

    lmic_event_queue = xQueueCreate(4, sizeof(ttn_lmic_event_t));
    ASSERT(lmic_event_queue != NULL);
    hal_esp32_start_lmic_task();
}

void ttn_set_subband(int band)
{
    subband = band;
}

void ttn_reset(void)
{
    hal_esp32_enter_critical_section();
    ttnState = TTN_DISCONNECTED;
    LMIC_reset();
    LMIC_setClockError(MAX_CLOCK_ERROR * 4 / 100);
    waiting_reason = TTN_WAITING_NONE;
    hal_esp32_leave_critical_section();
}

void ttn_shutdown(void)
{
    hal_esp32_enter_critical_section();
    ttnState = TTN_DISCONNECTED;
    LMIC_shutdown();
    hal_esp32_stop_lmic_task();
    waiting_reason = TTN_WAITING_NONE;
    hal_esp32_leave_critical_section();
}

void ttn_startup(void)
{
    hal_esp32_enter_critical_section();
    ttnState = TTN_DISCONNECTED;
    LMIC_reset();
    hal_esp32_start_lmic_task();
    hal_esp32_leave_critical_section();
}

bool ttn_provision(const char *dev_eui, const char *app_eui, const char *app_key)
{
    if (!ttn_provisioning_decode_keys(dev_eui, app_eui, app_key))
        return false;

    //return ttn_provisioning_save_keys();
    return true;
}

bool ttn_provision_with_mac(const char *app_eui, const char *app_key)
{
    if (!ttn_provisioning_from_mac(app_eui, app_key))
        return false;

    return ttn_provisioning_save_keys();
}

void ttn_start_provisioning_task(void)
{
#if defined(TTN_HAS_AT_COMMANDS)
    ttn_provisioning_start_task();
#else
    ESP_LOGE(TAG, "AT commands are disabled. Change the configuration using 'make menuconfig'");
    ASSERT(0);
    esp_restart();
#endif
}

void ttn_wait_for_provisioning(void)
{
#if defined(TTN_HAS_AT_COMMANDS)
    if (ttn_is_provisioned())
    {
        ESP_LOGI(TAG, "Device is already provisioned");
        return;
    }

    while (!ttn_provisioning_have_keys())
        vTaskDelay(pdMS_TO_TICKS(1000));

    ESP_LOGI(TAG, "Device successfully provisioned");
#else
    ESP_LOGE(TAG, "AT commands are disabled. Change the configuration using 'make menuconfig'");
    ASSERT(0);
    esp_restart();
#endif
}

bool ttn_join(const char *dev_eui, const char *app_eui, const char *app_key)
{
    if (!ttn_provisioning_decode_keys(dev_eui, app_eui, app_key))
        return false;

    return join_core();
}

bool ttn_join_provisioned(void)
{
    if (!ttn_provisioning_have_keys())
    {
        if (!ttn_provisioning_restore_keys(false))
            return false;
    }

    return join_core();
}

// Called immediately before sending join request message
void config_rf_params(void)
{
#if defined(CFG_us915) || defined(CFG_au915)
    if (subband != 0)
        LMIC_selectSubBand(subband - 1);
#endif

    if (join_data_rate != TTN_DR_JOIN_DEFAULT || max_tx_power != DEFAULT_MAX_TX_POWER)
    {
        dr_t dr = join_data_rate == TTN_DR_JOIN_DEFAULT ? LMIC.datarate : (dr_t) join_data_rate;
        s1_t txpow = max_tx_power == DEFAULT_MAX_TX_POWER ? LMIC.adrTxPow : max_tx_power;
        LMIC_setDrTxpow(dr, txpow);
    }
}

bool join_core(void)
{
    if (!ttn_provisioning_have_keys())
    {
        ESP_LOGW(TAG, "DevEUI, AppEUI/JoinEUI and/or AppKey have not been provided");
        return false;
    }

    is_started = true;
    hal_esp32_enter_critical_section();
    xQueueReset(lmic_event_queue);
    waiting_reason = TTN_WAITING_FOR_JOIN;

    LMIC_startJoining();
    config_rf_params();

    hal_esp32_wake_up();
    hal_esp32_leave_critical_section();

    ttn_lmic_event_t event;
    xQueueReceive(lmic_event_queue, &event, portMAX_DELAY);
    is_started = event.event == TTN_EVNT_JOIN_COMPLETED;
    ttnState = TTN_CONNECTED;
    return is_started;
}

ttn_response_code_t ttn_transmit_message(const uint8_t *payload, size_t length, ttn_port_t port, bool confirm)
{
    hal_esp32_enter_critical_section();
    if (waiting_reason != TTN_WAITING_NONE || (LMIC.opmode & OP_TXRXPEND) != 0)
    {
        hal_esp32_leave_critical_section();
        ttnState = TTN_DISCONNECTED;
        return TTN_ERROR_TRANSMISSION_FAILED;
    }

    waiting_reason = TTN_WAITING_FOR_TRANSMISSION;
    LMIC.client.txMessageCb = message_transmitted_callback;
    LMIC.client.txMessageUserData = NULL;
    LMIC_setTxData2(port, (xref2u1_t) payload, length, confirm);
    hal_esp32_wake_up();
    hal_esp32_leave_critical_section();

    while (true)
    {
        ttn_lmic_event_t result;
        xQueueReceive(lmic_event_queue, &result, portMAX_DELAY);

        switch (result.event)
        {
            case TTN_EVENT_MESSAGE_RECEIVED:
                ttnState = TTN_CONNECTED;
                if (message_callback != NULL)
                    message_callback(result.message, result.message_size, result.port);
            break;

            case TTN_EVENT_TRANSMISSION_COMPLETED:
                if (LMIC.opmode == 0x900)
                {
                    ttnState = TTN_CONNECTED;
                    return TTN_SUCCESSFUL_TRANSMISSION;
                }
            break;

            case TTN_EVENT_TRANSMISSION_FAILED:
                ttnState = TTN_DISCONNECTED;
                return TTN_ERROR_TRANSMISSION_FAILED;

            default:
                ASSERT(0);
        }
    }
}

void ttn_on_message(ttn_message_cb callback)
{
    message_callback = callback;
}

bool ttn_is_provisioned(void)
{
    if (ttn_provisioning_have_keys())
        return true;

    ttn_provisioning_restore_keys(true);

    return ttn_provisioning_have_keys();
}

void ttn_set_rssi_cal(int8_t rssi_cal)
{
    hal_esp32_set_rssi_cal(rssi_cal);
}

bool ttn_adr_enabled(void)
{
    return LMIC.adrEnabled != 0;
}

void ttn_set_adr_enabled(bool enabled)
{
    hal_esp32_enter_critical_section();
    LMIC_setAdrMode(enabled);
    hal_esp32_leave_critical_section();
}

void ttn_set_data_rate(ttn_data_rate_t data_rate)
{
    if (is_started)
    {
        hal_esp32_enter_critical_section();
        LMIC_setDrTxpow(data_rate, LMIC.adrTxPow);
        hal_esp32_leave_critical_section();
    }
    else
    {
        join_data_rate = data_rate;
    }
}

void ttn_set_max_tx_pow(int tx_pow)
{
    if (is_started)
    {
        hal_esp32_enter_critical_section();
        LMIC_setDrTxpow(LMIC.datarate, tx_pow);
        hal_esp32_leave_critical_section();
    }
    else
    {
        max_tx_power = tx_pow;
    }
}

ttn_rf_settings_t ttn_getrf_settings(ttn_rx_tx_window_t window)
{
    int index = ((int) window) & 0x03;
    return last_rf_settings[index];
}

ttn_rf_settings_t ttn_tx_settings(void)
{
    return last_rf_settings[TTN_WINDOW_TX];
}

ttn_rf_settings_t ttn_rx1_settings(void)
{
    return last_rf_settings[TTN_WINDOW_RX1];
}

ttn_rf_settings_t ttn_rx2_settings(void)
{
    return last_rf_settings[TTN_WINDOW_RX2];
}

ttn_rx_tx_window_t ttn_rx_tx_window(void)
{
    return current_rx_tx_window;
}

int ttn_rssi(void)
{
    return LMIC.rssi;
}

// --- Callbacks ---

#if CONFIG_LOG_DEFAULT_LEVEL >= 3 || LMIC_ENABLE_event_logging || LMIC_DISPLAY_ENABLED > 0
static const char *event_names[] = { LMIC_EVENT_NAME_TABLE__INIT };
#endif

// Called by LMIC when an LMIC event (join, join failed, reset etc.) occurs
void event_callback(void *user_data, ev_t event)
{
    // update monitoring information
    switch (event)
    {
        case EV_TXSTART:
            current_rx_tx_window = TTN_WINDOW_TX;
            save_rf_settings(&last_rf_settings[TTN_WINDOW_TX]);
            clear_rf_settings(&last_rf_settings[TTN_WINDOW_RX1]);
            clear_rf_settings(&last_rf_settings[TTN_WINDOW_RX2]);
        break;

        case EV_RXSTART:
            if (current_rx_tx_window != TTN_WINDOW_RX1)
            {
                current_rx_tx_window = TTN_WINDOW_RX1;
                save_rf_settings(&last_rf_settings[TTN_WINDOW_RX1]);
            }
            else
            {
                current_rx_tx_window = TTN_WINDOW_RX2;
                save_rf_settings(&last_rf_settings[TTN_WINDOW_RX2]);
            }
        break;

        default:
            current_rx_tx_window = TTN_WINDOW_IDLE;
        break;
    };

#if LMIC_ENABLE_event_logging
    ttn_log_event(event, event_names[event], 0);
#elif CONFIG_LOG_DEFAULT_LEVEL >= 3
    ESP_LOGI(TAG, "event %s", event_names[event]);
    LMIC_DEBUG_PRINTF("%"LMIC_PRId_ostime_t": engineUpdate, opmode=0x%x\n", os_getTime(), LMIC.opmode);
#endif

#if LMIC_DISPLAY_ENABLED == 1
    //Debug display info
    char dispStr[24];
    u2_t op;
    hal_esp32_enter_critical_section();
    op = LMIC.opmode;
    hal_esp32_leave_critical_section();
    sprintf(dispStr, (char*) event_names[event]);
    sprintf(dispStr + strlen(dispStr), " 0x%x", op);
    WriteDisplay(dispStr, 5);
    if (event == EV_TXSTART)
    {
        ResetSeconds();
        sprintf(dispStr, "%s:%.1fMHz SF%d", "TX", LMIC.freq / 1000000.0, getSf(LMIC.rps) + 6);
        WriteDisplay(dispStr, 6);
        sprintf(dispStr, "POWER:%+ddBm DR%d", LMIC.radio_txpow, LMIC.datarate);
        WriteDisplay(dispStr, 7);
    }
    else if (event == EV_RXSTART)
    {
        sprintf(dispStr, "%s:%.1fMHz SF%d", "RX", LMIC.freq / 1000000.0, getSf(LMIC.rps) + 6);
        WriteDisplay(dispStr, 6);
        WriteDisplay(" ", 7);
    }
    else if (event == EV_TXCOMPLETE)
    {
        sprintf(dispStr, "RSSI:%+ddBm SNR:%+ddB", LMIC.rssi - RSSI_OFF, LMIC.snr / 4);
        WriteDisplay(dispStr, 7);
    }
    UpdateDisplay();
#endif

    ttn_event_t ttn_event = TTN_EVENT_NONE;

    if (waiting_reason == TTN_WAITING_FOR_JOIN)
    {
        if (event == EV_JOINED)
        {
            ttn_event = TTN_EVNT_JOIN_COMPLETED;
        }
        else if (event == EV_REJOIN_FAILED || event == EV_RESET)
        {
            ttn_event = TTN_EVENT_JOIN_FAILED;
        }
    }

    if (event == EV_TXCOMPLETE && LMIC.opmode == 0x900)
    {
        ttn_event = TTN_EVENT_TRANSMISSION_COMPLETED;
    }

    if (ttn_event == TTN_EVENT_NONE)
        return;

    ttn_lmic_event_t result = { .event = ttn_event };
    waiting_reason = TTN_WAITING_NONE;
    xQueueSend(lmic_event_queue, &result, pdMS_TO_TICKS(100));
}

// Called by LMIC when a message has been received
void message_received_callback(void *user_data, uint8_t port, const uint8_t *message, size_t message_size)
{
    ttn_lmic_event_t result = {
            .event = TTN_EVENT_MESSAGE_RECEIVED, .port = port, .message = message, .message_size = message_size };
    xQueueSend(lmic_event_queue, &result, pdMS_TO_TICKS(100));
}

// Called by LMIC when a message has been transmitted (or the transmission failed)
void message_transmitted_callback(void *user_data, int success)
{
    waiting_reason = TTN_WAITING_NONE;
    ttn_lmic_event_t result = { .event = success ? TTN_EVENT_TRANSMISSION_COMPLETED : TTN_EVENT_TRANSMISSION_FAILED };
    xQueueSend(lmic_event_queue, &result, pdMS_TO_TICKS(100));
}

// --- Helpers

void save_rf_settings(ttn_rf_settings_t *rf_settings)
{
    rf_settings->spreading_factor = (ttn_spreading_factor_t) (getSf(LMIC.rps) + 1);
    rf_settings->bandwidth = (ttn_bandwidth_t) (getBw(LMIC.rps) + 1);
    rf_settings->frequency = LMIC.freq;
}

void clear_rf_settings(ttn_rf_settings_t *rf_settings)
{
    memset(rf_settings, 0, sizeof(*rf_settings));
}

/*My added functions*/
bool ttn_is_connected(void)
{
    return ttnState;
}

void ttn_rejoin(void)
{
    LMIC_tryRejoin();
}
