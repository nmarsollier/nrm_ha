/* USB Net — usb_net_descriptors.c — NCM descriptors with Microsoft OS 2.0.
 *
 * The auto-generated NCM descriptor from esp_tinyusb is enough for macOS and
 * Linux, but Windows 10/11 does not bind its NCM class driver (usbncm.sys)
 * unless the device advertises a Microsoft OS 2.0 descriptor that associates
 * the NCM function with the "WINNCM" compatible ID.  This file supplies the
 * three missing pieces:
 *
 *   - a custom device descriptor (bcdUSB 2.1, MISC/IAD class) required to
 *     expose a BOS descriptor;
 *   - the BOS descriptor that points at the MS OS 2.0 platform capability;
 *   - the MS OS 2.0 descriptor set plus the vendor control handler that
 *     serves it.
 *
 * tud_descriptor_bos_cb() and tud_vendor_control_xfer_cb() are weak symbols
 * in the TinyUSB core that esp_tinyusb does not override, so defining them
 * here installs the Windows support without touching the component.
 */
#include "usb_net_internal.h"

#include "esp_log.h"

#include "tinyusb.h"

static const char *TAG = "USB_NET_DESCRIPTORS";

/* ── Microsoft OS 2.0 descriptor constants ───────────────────────── */

#define USB_NET_MS_OS_VENDOR_CODE   1       /* vendor code advertised in the BOS descriptor */
#define USB_NET_MS_OS_DESC_INDEX    7       /* wIndex Windows uses to fetch the MS OS 2.0 descriptor */
#define USB_NET_MS_OS_20_DESC_LEN   0xB2    /* total size of the descriptor set, in bytes */
#define USB_NET_BOS_TOTAL_LEN       (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)
#define USB_NET_NCM_ITF_NUM         0       /* NCM control interface number in the generated descriptor */

/* Custom device descriptor.
 *
 * Two differences from the esp_tinyusb default:
 *   - bcdUSB 2.1 so the host requests the BOS descriptor;
 *   - device class MISC / IAD, as required for a device whose configuration
 *     descriptor carries an Interface Association Descriptor (the NCM IAD).
 */
static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0201,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = CONFIG_TINYUSB_DESC_CUSTOM_VID,
    .idProduct          = CONFIG_TINYUSB_DESC_CUSTOM_PID,
    .bcdDevice          = CONFIG_TINYUSB_DESC_BCD_DEVICE,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

/* Binary Device Object Store descriptor: a single Microsoft OS 2.0 platform
 * capability. */
static const uint8_t s_bos_desc[] = {
    TUD_BOS_DESCRIPTOR(USB_NET_BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(USB_NET_MS_OS_20_DESC_LEN, USB_NET_MS_OS_VENDOR_CODE),
};

/* Microsoft OS 2.0 descriptor set (see the TinyUSB net_lwip_webserver example
 * and Microsoft's "Microsoft OS 2.0 Descriptors Specification").
 *
 * It declares a compatible ID of "WINNCM" for the NCM function and a
 * DeviceInterfaceGUIDs registry property, which makes Windows auto-load
 * usbncm.sys without a manual driver install. */
static const uint8_t s_ms_os_20_desc[] = {
    /* Set header: wLength, wDescriptorType, dwWindowsVersion, wTotalLength */
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(USB_NET_MS_OS_20_DESC_LEN),

    /* Configuration subset header: configuration index 0 */
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(USB_NET_MS_OS_20_DESC_LEN - 0x0A),

    /* Function subset header: first interface of the NCM function */
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    USB_NET_NCM_ITF_NUM, 0, U16_TO_U8S_LE(USB_NET_MS_OS_20_DESC_LEN - 0x0A - 0x08),

    /* Compatible ID: "WINNCM" (the usbncm.sys class driver) */
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'N', 'C', 'M', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* Registry property: DeviceInterfaceGUIDs (multisz) */
    U16_TO_U8S_LE(USB_NET_MS_OS_20_DESC_LEN - 0x0A - 0x08 - 0x08 - 0x14),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), /* data type, name length ("DeviceInterfaceGUIDs\0") */
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00,
    't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00,
    'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050), /* wPropertyDataLength */
    /* {12345678-0D08-43FD-8B3E-127CA8AFFF9D} */
    '{', 0x00, '1', 0x00, '2', 0x00, '3', 0x00, '4', 0x00, '5', 0x00, '6', 0x00, '7', 0x00,
    '8', 0x00, '-', 0x00, '0', 0x00, 'D', 0x00, '0', 0x00, '8', 0x00, '-', 0x00, '4', 0x00,
    '3', 0x00, 'F', 0x00, 'D', 0x00, '-', 0x00, '8', 0x00, 'B', 0x00, '3', 0x00, 'E', 0x00,
    '-', 0x00, '1', 0x00, '2', 0x00, '7', 0x00, 'C', 0x00, 'A', 0x00, '8', 0x00, 'A', 0x00,
    'F', 0x00, 'F', 0x00, 'F', 0x00, '9', 0x00, 'D', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
};

TU_VERIFY_STATIC(sizeof(s_ms_os_20_desc) == USB_NET_MS_OS_20_DESC_LEN,
                 "Incorrect MS OS 2.0 descriptor size");

/* ── Accessors / TinyUSB callbacks ───────────────────────────────── */

const tusb_desc_device_t *usb_net_device_descriptor(void)
{
    return &s_device_desc;
}

uint8_t const *tud_descriptor_bos_cb(void)
{
    return s_bos_desc;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    /* Windows fetches the MS OS 2.0 descriptor with the vendor request
     * bRequest = USB_NET_MS_OS_VENDOR_CODE, wIndex = USB_NET_MS_OS_DESC_INDEX. */
    if (request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR ||
        request->bRequest != USB_NET_MS_OS_VENDOR_CODE ||
        request->wIndex != USB_NET_MS_OS_DESC_INDEX) {
        return false;
    }

    ESP_LOGI(TAG, "serving Microsoft OS 2.0 descriptor to Windows host");
    return tud_control_xfer(rhport, request, (void *)(uintptr_t)s_ms_os_20_desc,
                            (uint16_t)sizeof(s_ms_os_20_desc));
}
