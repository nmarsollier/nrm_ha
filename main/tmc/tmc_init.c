/* TMC — tmc_init.c
 *
 * Purpose: initialise two TMC2209 stepper drivers, each over its own
 *          single-wire UART bus, and configure microstepping, current,
 *          and chopper mode.
 *
 * Each driver has a dedicated UART (TX + RX GPIO pair per driver), so
 * both are addressed 0x00 (MS1 and MS2 tied to GND).  The single-wire
 * bus connects TX through a 1 kΩ series resistor to the TMC2209
 * PDN_UART pin, and RX directly to that pin; TX is floated while the
 * driver's response is read (see tmc_read_register).
 */
#include "tmc.h"
#include "tmc_internal.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/task.h"

/* ── UART hardware ─────────────────────────────────────────── */

#define TMC_BAUD_RATE    115200
#define TMC2209_ADDRESS  0x00   /* both drivers: MS1=GND, MS2=GND */

/* ── TMC2209 register addresses ────────────────────────────── */

#define TMC_REG_GCONF      0x00
#define TMC_REG_IHOLD_IRUN 0x10
#define TMC_REG_CHOPCONF   0x6C
#define TMC_REG_TPWMTHRS   0x13
#define TMC_REG_DRV_STATUS 0x6F

/* StealthChop→SpreadCycle auto-transition threshold.
 *
 * A nonzero value makes the driver switch from StealthChop to SpreadCycle
 * once TSTEP < TPWMTHRS.  This is a safety net for a SPREAD pin wired HIGH
 * (which inverts GCONF.en_SpreadCycle back to StealthChop):
 *  - SPREAD LOW  + en_SpreadCycle=1 → always SpreadCycle (threshold ignored)
 *  - SPREAD HIGH + en_SpreadCycle=1 → StealthChop only below the threshold,
 *    SpreadCycle above (threshold flips it)
 * Threshold: f_STEP = fCLK × µ / (256 × TPWMTHRS).  With fCLK=12 MHz,
 * µ=32 and 5000 → ~300 input steps/s ≈ 0.06 deg/s on this mount. */
#define TMC_TPWMTHRS_VALUE 5000

/* ── Target microstep resolution — see TMC_TARGET_MICROSTEPS in tmc.h ── */

/* Convert TMC_TARGET_MICROSTEPS to the MRES register field.
 * TMC2209 MRES mapping: 256→0, 128→1, 64→2, 32→3, 16→4, 8→5, 4→6, 2→7, 1→8 */
static uint8_t tmc_microsteps_to_mres(uint16_t ms) {
    switch (ms) {
        case 256: return 0;
        case 128: return 1;
        case 64:  return 2;
        case 32:  return 3;
        case 16:  return 4;
        case 8:   return 5;
        case 4:   return 6;
        case 2:   return 7;
        default:  return 8;
    }
}

/* Convert MRES register field back to microsteps — used for read-back verification. */
static bool tmc_mres_to_microsteps(uint8_t mres, uint16_t *microsteps) {
    switch (mres) {
        case 0:  *microsteps = 256; return true;
        case 1:  *microsteps = 128; return true;
        case 2:  *microsteps = 64;  return true;
        case 3:  *microsteps = 32;  return true;
        case 4:  *microsteps = 16;  return true;
        case 5:  *microsteps = 8;   return true;
        case 6:  *microsteps = 4;   return true;
        case 7:  *microsteps = 2;   return true;
        case 8:  *microsteps = 1;   return true;
        default: return false;
    }
}

static const char *TAG = "TMC_INIT";

/* ── Per-axis configuration ────────────────────────────────── */

typedef struct {
    const char *name;
    uart_port_t uart_num;
    gpio_num_t  tx_gpio;     /* TX → 1 kΩ → PDN_UART line */
    gpio_num_t  rx_gpio;     /* RX → PDN_UART line (direct) */
    uint8_t     irun;        /* run current  (0 – 31) */
    uint8_t     ihold;       /* hold current (0 – 31) */
} TmcAxis;

static const TmcAxis tmc_axes[] = {
    { .name = "RA",  .uart_num = 1, .tx_gpio = GPIO_NUM_21, .rx_gpio = GPIO_NUM_12, .irun = 10, .ihold = 8 },
    { .name = "DEC", .uart_num = 2, .tx_gpio = GPIO_NUM_2,  .rx_gpio = GPIO_NUM_9,  .irun = 10, .ihold = 8 },
};

/* ── UART helpers ──────────────────────────────────────────── */

static uint8_t tmc_crc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (((crc >> 7) ^ (byte & 0x01)) != 0) {
                crc = (crc << 1) ^ 0x07;
            } else {
                crc <<= 1;
            }
            byte >>= 1;
        }
    }
    return crc;
}

/*
 * Write a TMC2209 register over its dedicated single-wire UART bus.
 *
 * The written frame loops back into the RX FIFO (RX is on the same wire);
 * the echo is drained here so it never leaks into a subsequent read.
 */
static esp_err_t tmc_write_register(const TmcAxis *axis, uint8_t reg, uint32_t value)
{
    uint8_t request[8] = {
        0x05,
        TMC2209_ADDRESS,
        (uint8_t)(reg | 0x80),
        (uint8_t)(value >> 24),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),
        (uint8_t)(value),
        0x00,
    };
    request[7] = tmc_crc(request, 7);

    uart_flush_input(axis->uart_num);

    int written = uart_write_bytes(axis->uart_num, request, sizeof(request));
    if (written != (int)sizeof(request)) {
        return ESP_FAIL;
    }

    uart_wait_tx_done(axis->uart_num, pdMS_TO_TICKS(10));

    /* Absorb the 8-byte loopback echo from the single-wire bus. */
    uint8_t echo_buffer[8];
    uart_read_bytes(axis->uart_num, echo_buffer, sizeof(request), pdMS_TO_TICKS(10));

    return ESP_OK;
}

/*
 * Read a TMC2209 register over its dedicated single-wire UART bus.
 *
 * TX connects through a 1 kΩ series resistor to the TMC2209 PDN_UART
 * pin, and RX connects directly to that pin.  After sending the read
 * request and draining the loopback echo, TX is floated (switched to
 * input) so the TMC2209 output can drive the line cleanly for its 8-byte
 * response — the internal pull-up holds the idle HIGH.
 *
 * Retries up to 2 additional times on timeout or invalid response —
 * single-wire UART reads are inherently prone to occasional corruption.
 */
static esp_err_t tmc_read_register(const TmcAxis *axis, uint8_t reg, uint32_t *value)
{
    if (value == NULL) return ESP_ERR_INVALID_ARG;

    uint8_t request[4] = {
        0x05,
        TMC2209_ADDRESS,
        (uint8_t)(reg & 0x7F),   /* bit 7 = 0 → read */
        0x00,
    };
    request[3] = tmc_crc(request, 3);

    esp_err_t last_err = ESP_FAIL;

    for (int attempt = 0; attempt < 6; attempt++) {
        if (attempt > 0)
            vTaskDelay(pdMS_TO_TICKS(10));

        uart_flush_input(axis->uart_num);

        int written = uart_write_bytes(axis->uart_num, request, sizeof(request));
        if (written != (int)sizeof(request)) return ESP_FAIL;

        uart_wait_tx_done(axis->uart_num, pdMS_TO_TICKS(10));

        /* Drain the 4-byte loopback echo before floating TX. */
        uint8_t echo_buf[4];
        uart_read_bytes(axis->uart_num, echo_buf, sizeof(request), pdMS_TO_TICKS(10));

        /*
         * Float TX so the TMC2209 output has a clean, high-impedance
         * line to drive for the remainder of its 8-byte response frame
         * (the internal pull-up holds the idle HIGH level).
         */
        gpio_set_direction(axis->tx_gpio, GPIO_MODE_INPUT);
        gpio_set_pull_mode(axis->tx_gpio, GPIO_PULLUP_ONLY);

        /* Collect the remaining bytes of the combined (echo + reply)
         * stream.  Up to 32 bytes are buffered to tolerate a few extra
         * framing / noise bytes. */
        uint8_t response[32];
        int len = 0;
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(50);

        while (xTaskGetTickCount() < deadline && len < (int)sizeof(response)) {
            int n = uart_read_bytes(axis->uart_num, response + len,
                                    sizeof(response) - len, pdMS_TO_TICKS(5));
            if (n > 0) len += n;
        }

        /* Restore TX — drive HIGH before re-enabling the output so the
         * bus doesn't glitch LOW while the IO MUX is reconnected. */
        gpio_set_level(axis->tx_gpio, 1);
        gpio_set_direction(axis->tx_gpio, GPIO_MODE_OUTPUT);
        uart_set_pin(axis->uart_num, axis->tx_gpio, axis->rx_gpio,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

        if (len < 8) {
            last_err = ESP_ERR_TIMEOUT;
            continue;
        }

        /* Find and validate a frame in the response buffer. */
        for (int i = 0; i <= len - 8; i++) {
            uint8_t *frame = &response[i];
            if (frame[0] != 0x05) continue;
            if ((frame[2] & 0x7F) != (reg & 0x7F)) continue;
            if (tmc_crc(frame, 7) != frame[7]) continue;

            *value = ((uint32_t)frame[3] << 24) |
                     ((uint32_t)frame[4] << 16) |
                     ((uint32_t)frame[5] << 8)  |
                     ((uint32_t)frame[6]);
            return ESP_OK;
        }

        last_err = ESP_ERR_INVALID_RESPONSE;
    }

    return last_err;
}

/* ── Driver init (write + verify) ──────────────────────────── */

static esp_err_t tmc_init_driver(const TmcAxis *axis, int axis_index)
{
    esp_err_t result;
    uint32_t verify = 0;

    ESP_LOGI(TAG, "--- Initialising axis %s [UART %d, TX %d, RX %d] ---",
             axis->name, axis->uart_num, axis->tx_gpio, axis->rx_gpio);

    /* ── GCONF: force SpreadCycle (StealthChop off) ──
     * en_SpreadCycle (bit 2) = 1 selects SpreadCycle when the SPREAD pin
     * is LOW (SPREAD HIGH inverts this bit). mstep_reg_select (bit 7) = 1
     * takes microsteps from CHOPCONF.MRES; pdn_disable (bit 6) = 1 keeps
     * PDN_UART as a pure UART. */
    uint32_t gconf = 0x000000C4;
    bool gconf_ok = false;
    for (int attempt = 0; attempt < 5; attempt++) {
        result = tmc_write_register(axis, TMC_REG_GCONF, gconf);
        if (result == ESP_OK) {
            result = tmc_read_register(axis, TMC_REG_GCONF, &verify);
            if (result == ESP_OK && (verify & (1U << 7)) && (verify & (1U << 2))) {
                gconf_ok = true;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (gconf_ok) {
        ESP_LOGI(TAG, "%s: GCONF verified — en_SpreadCycle=1 (StealthChop off), mstep_reg_select=1 (0x%08lX)",
                 axis->name, (unsigned long)verify);
    } else {
        ESP_LOGW(TAG, "%s: GCONF not latched after retries — chopper mode UNVERIFIED", axis->name);
    }

    /* ── IHOLD_IRUN ──
     * Field layout: IHOLD [4:0], IRUN [12:8], IHOLDDELAY [19:16]. */
    uint32_t ihold_irun = (uint32_t)axis->ihold        /* IHOLD [4:0] */
                        | ((uint32_t)axis->irun << 8)  /* IRUN  [12:8] */
                        | (1U << 16);                  /* IHOLDDELAY = 1 */
    result = tmc_write_register(axis, TMC_REG_IHOLD_IRUN, ihold_irun);
    if (result != ESP_OK) {
        tmc_axis_status[axis_index] = TMC_AXIS_ERROR;
        tmc_axis_error[axis_index] = TMC_AXIS_ERROR_IHOLD_WRITE;
        ESP_LOGE(TAG, "%s: UART write failure on IHOLD_IRUN", axis->name);
        return result;
    }

    /* ── CHOPCONF ── */
    uint32_t chopconf = 0x1041015A;      /* TOFF=10 (~10 kHz chopper) */
    chopconf &= ~(0x0FU << 24);          /* clear MRES */
    chopconf |=  ((uint32_t)tmc_microsteps_to_mres(TMC_TARGET_MICROSTEPS) << 24);
    chopconf |=  (1U << 28);             /* intpol → 256 µsteps */

    /* ── CHOPCONF: write + verify (retry — single-wire UART readback is flaky) ── */
    uint16_t verified_msteps = 0;
    bool chopconf_ok = false;
    for (int attempt = 0; attempt < 5; attempt++) {
        result = tmc_write_register(axis, TMC_REG_CHOPCONF, chopconf);
        if (result == ESP_OK) {
            result = tmc_read_register(axis, TMC_REG_CHOPCONF, &verify);
            if (result == ESP_OK) {
                uint8_t mres = (uint8_t)((verify >> 24) & 0x0F);
                if (tmc_mres_to_microsteps(mres, &verified_msteps)
                    && verified_msteps == TMC_TARGET_MICROSTEPS) {
                    chopconf_ok = true;
                    break;
                }
                ESP_LOGW(TAG, "%s: CHOPCONF mismatch on attempt %d (0x%08lX) — re-writing",
                         axis->name, attempt, (unsigned long)verify);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }

    if (!chopconf_ok) {
        tmc_axis_status[axis_index] = TMC_AXIS_ERROR;
        tmc_axis_error[axis_index] = TMC_AXIS_ERROR_CHOPCONF_VERIFY;
        ESP_LOGE(TAG, "%s: CHOPCONF could not be verified after retries", axis->name);
        return ESP_FAIL;
    }

    bool intpol_ok = (verify & (1U << 28)) != 0;

    ESP_LOGI(TAG, "%s: CHOPCONF verified — %u µsteps, intpol=%s (0x%08lX)",
             axis->name, verified_msteps,
             intpol_ok ? "ON" : "OFF",
             (unsigned long)verify);
    /* ── TPWMTHRS: force the StealthChop→SpreadCycle velocity switch as a
     * safety net for a SPREAD pin wired HIGH (which inverts GCONF.
     * en_SpreadCycle back to StealthChop).  With SPREAD LOW this threshold
     * is ignored (the driver is already in SpreadCycle). */
    result = tmc_write_register(axis, TMC_REG_TPWMTHRS, TMC_TPWMTHRS_VALUE);
    if (result != ESP_OK) {
        tmc_axis_status[axis_index] = TMC_AXIS_ERROR;
        tmc_axis_error[axis_index] = TMC_AXIS_ERROR_TPWMTHRS_WRITE;
        ESP_LOGE(TAG, "%s: UART write failure on TPWMTHRS", axis->name);
        return result;
    }

    /* ── DRV_STATUS: confirm the REAL chopper mode ──
     * bit 30 (stealth): 0 = SpreadCycle, 1 = StealthChop. This reflects
     * the actual mode after any SPREAD-pin inversion, catching a SPREAD
     * pin wired HIGH (which inverts GCONF.en_SpreadCycle). */
    if (tmc_read_register(axis, TMC_REG_DRV_STATUS, &verify) == ESP_OK) {
        bool stealth = (verify & (1U << 30)) != 0;
        ESP_LOGI(TAG, "%s: DRV_STATUS=0x%08lX — running in %s",
                 axis->name, (unsigned long)verify,
                 stealth ? "StealthChop" : "SpreadCycle");
        if (stealth) {
            ESP_LOGW(TAG, "%s: StealthChop active despite en_SpreadCycle=1 — check SPREAD pin (HIGH inverts)",
                     axis->name);
        }
    } else {
        ESP_LOGW(TAG, "%s: DRV_STATUS read failed — real chopper mode unknown", axis->name);
    }

    tmc2209_set_active_microsteps(verified_msteps);
    tmc_axis_status[axis_index] = TMC_AXIS_OK;
    tmc_axis_error[axis_index] = TMC_AXIS_ERROR_NONE;

    ESP_LOGI(TAG, "%s: init complete (cached µsteps=%u, irun=%u, ihold=%u)",
             axis->name, verified_msteps, axis->irun, axis->ihold);
    return ESP_OK;
}

/* ── Public entry point ────────────────────────────────────── */

esp_err_t tmc2209_hw_init(void)
{
    esp_err_t result;

    /* Reset per-axis diagnostics before (re)initialising. */
    for (int i = 0; i < TMC_AXIS_COUNT; i++) {
        tmc_axis_status[i] = TMC_AXIS_NOT_INIT;
        tmc_axis_error[i] = TMC_AXIS_ERROR_NONE;
    }

    /* Install and configure each driver's dedicated single-wire UART. */
    for (size_t i = 0; i < sizeof(tmc_axes) / sizeof(tmc_axes[0]); i++) {
        const TmcAxis *axis = &tmc_axes[i];

        uart_config_t config = {
            .baud_rate  = TMC_BAUD_RATE,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };

        result = uart_driver_install(axis->uart_num, 512, 512, 0, NULL, 0);
        if (result != ESP_OK) {
            tmc_axis_status[i] = TMC_AXIS_ERROR;
            tmc_axis_error[i] = TMC_AXIS_ERROR_UART;
            ESP_LOGE(TAG, "%s: uart_driver_install failed: %s",
                     axis->name, esp_err_to_name(result));
            continue;
        }

        result = uart_param_config(axis->uart_num, &config);
        if (result != ESP_OK) {
            tmc_axis_status[i] = TMC_AXIS_ERROR;
            tmc_axis_error[i] = TMC_AXIS_ERROR_UART;
            ESP_LOGE(TAG, "%s: uart_param_config failed: %s",
                     axis->name, esp_err_to_name(result));
            continue;
        }

        /* Single-wire: TX through 1 kΩ, RX direct to the PDN_UART line. */
        result = uart_set_pin(axis->uart_num, axis->tx_gpio, axis->rx_gpio,
                              UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (result != ESP_OK) {
            tmc_axis_status[i] = TMC_AXIS_ERROR;
            tmc_axis_error[i] = TMC_AXIS_ERROR_UART;
            ESP_LOGE(TAG, "%s: uart_set_pin failed: %s",
                     axis->name, esp_err_to_name(result));
            continue;
        }

        gpio_set_pull_mode(axis->rx_gpio, GPIO_PULLUP_ONLY);
    }

    /* Let the buses settle before writing to the drivers. */
    vTaskDelay(pdMS_TO_TICKS(20));

    bool all_ok = true;
    for (size_t i = 0; i < sizeof(tmc_axes) / sizeof(tmc_axes[0]); i++) {
        if (tmc_axis_status[i] == TMC_AXIS_ERROR) {
            all_ok = false;  /* UART setup already failed for this axis */
            continue;
        }
        result = tmc_init_driver(&tmc_axes[i], (int)i);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Error initialising axis %s", tmc_axes[i].name);
            all_ok = false;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }

    if (!all_ok) {
        ESP_LOGE(TAG, "One or more axes failed UART verification — microsteps not trusted, mount in error state");
        tmc2209_set_active_microsteps(0);
        return ESP_FAIL;
    }

    return ESP_OK;
}
