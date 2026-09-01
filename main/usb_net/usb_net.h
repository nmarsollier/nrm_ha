#pragma once

#include "esp_err.h"

/*
 * USB Ethernet (NCM) initialisation.
 *
 * Configures the ESP32-S3 USB OTG peripheral as an NCM network device
 * via TinyUSB.  The host computer sees a standard USB Ethernet adapter
 * and receives an IP address via DHCP.
 *
 * ESP32-S3 static IP: 192.168.7.1
 * DHCP pool:          192.168.7.2 – 192.168.7.10
 *
 * USB networking is the device's only network interface — servers bound
 * to INADDR_ANY are reachable over it.
 *
 * Call once during startup, after esp_netif_init() and led_init().
 * Failure is logged but is not fatal.
 */
esp_err_t usb_net_init(void);
