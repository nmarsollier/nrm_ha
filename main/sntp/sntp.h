#pragma once

/*
 * SNTP — network time synchronisation.
 *
 * Starts a background SNTP client that keeps the system clock in UTC.  The
 * clock is seeded from a network time server when one is reachable over the
 * USB Ethernet link; if none responds, the mount falls back to time set by
 * the client (Alpaca UTCDate) or the embedded web UI.
 *
 * Call once during startup, after usb_net_init().
 */
void sntp_start(void);
