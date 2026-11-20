#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "actuator_client.h"
#include "sensor_client.h"
#include "thread_commissioner.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Khoi tao toan bo OpenThread Border Router stack, Spinel UART RCP (TX:13, RX:14 @ 460800bps),
 *        khoi phuc hoac tu sinh mang Thread bao mat va bat SRP Server & CoAP clients.
 */
esp_err_t thread_gateway_init(void);

/**
 * @brief Gui lenh dieu khien Cong tac Relay qua CoAP Confirmable PUT /relay.
 */
esp_err_t thread_gateway_set_relay(bool on);

/**
 * @brief Gui lenh dieu khien Den Dimmer (0 - 100%) qua CoAP Confirmable PUT /light.
 */
esp_err_t thread_gateway_set_dimmer(uint8_t brightness);

/**
 * @brief Kiem tra co-processor ESP32-H2 RCP da ket noi va phan hoi Spinel UART thanh cong hay chua.
 */
bool thread_gateway_is_rcp_connected(void);

/**
 * @brief Dem so luong thiet bi Thread dang online trong bang SRP Server.
 */
uint8_t thread_gateway_get_node_count(void);

/**
 * @brief Kiem tra trang thai online truc tiep cua tung thiet bi qua SRP Server.
 */
bool thread_gateway_is_relay_online(void);
bool thread_gateway_is_dimmer_online(void);
bool thread_gateway_is_sensor_online(void);

/**
 * @brief Dang ky callback nhan du lieu DHT11 tu sensor_client.
 */
void thread_gateway_set_sensor_callback(sensor_callback_t cb);

/**
 * @brief Dang ky callback nhan phan hoi bat/tat hoac do sang tu actuator_client.
 */
void thread_gateway_set_actuator_callback(actuator_callback_t cb);

typedef void (*thread_report_callback_t)(const char *payload);
void thread_gateway_set_report_callback(thread_report_callback_t cb);

/**
 * @brief Go bo thiet bi khoi mang Thread va xoa khoi bang dang ky NVS.
 */
esp_err_t thread_gateway_remove_device(const char *dev_id_str);

#ifdef __cplusplus
}
#endif
