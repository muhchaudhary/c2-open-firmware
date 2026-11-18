#!/usr/bin/env python3
"""patch_sdk.py: build the gamepad firmware in a fw-AC63_BT_SDK checkout
(called by firmware/sdk/sync.sh, after it installed the sources).

  patch_sdk.py SDK_DIR BOARD

The firmware is installed at SDK_DIR/apps/gamepad (src/ and boards/BOARD/)
and built as part of apps/hid's br23 build: this adds its sources and
include paths to that Makefile, selects the board, and makes the SDK changes
the firmware needs. Each patch replaces one exact piece of text, once; a
missing anchor is a hard error (the SDK checkout differs from the one this
was written for, firmware/sdk/SDK_COMMIT; sync.sh checks it first). Everything beyond board selection sits behind
#ifdef CONFIG_GAMEPAD_FW (defined by the board's config header), so the
SDK's own boards build as before. sync.sh restores the patched files from
git first, so this always starts from the SDK's own text.
"""
import os
import re
import sys

SDK, BOARD = sys.argv[1], sys.argv[2]
UP = '../../../..'                      # from apps/hid/board/br23 to the SDK root
GP = 'apps/gamepad'


def patch(rel, old, new, what):
    path = f'{SDK}/{rel}'
    s = open(path, encoding='utf-8').read()
    if new in s:
        return
    if s.count(old) != 1:
        sys.exit(f'error: {rel}: anchor for "{what}" found {s.count(old)} times (want 1)')
    open(path, 'w', encoding='utf-8').write(s.replace(old, new))
    print(f'  {rel}: {what}')


BR23 = 'apps/hid/board/br23'
MK = f'{BR23}/Makefile'

# --- the firmware's sources and include paths --------------------------------
srcs = []
# A board may list the firmware/src files it builds in boards/<name>/sources
# (one path under src/ per line, # comments): the receiver builds its own app
# and only some of the shared files. Without that file, all of src/.
only = None
listing = f'{SDK}/{GP}/boards/{BOARD}/sources'
if os.path.exists(listing):
    only = {l.split('#')[0].strip() for l in open(listing)} - {''}
    for o in only:
        if not os.path.exists(f'{SDK}/{GP}/src/{o}'):
            sys.exit(f'error: boards/{BOARD}/sources lists src/{o}, which does not exist')
for top in (f'{GP}/src', f'{GP}/boards/{BOARD}'):
    for d, _, files in os.walk(f'{SDK}/{top}'):
        for f in files:
            rel = os.path.relpath(f'{d}/{f}', f'{SDK}/{GP}/src')
            if f.endswith('.c') and (only is None or top != f'{GP}/src' or rel in only):
                srcs.append(os.path.relpath(f'{d}/{f}', SDK))
incs = [f'{GP}/src', f'{GP}/src/core', f'{GP}/src/drivers', f'{GP}/boards/{BOARD}']
patch(MK, f'\t-I{UP}/apps/hid/board/br23 \\\n',
      f'\t-I{UP}/apps/hid/board/br23 \\\n' + ''.join(f'\t-I{UP}/{i} \\\n' for i in incs),
      'gamepad include paths')
patch(MK, f'\t{UP}/apps/hid/board/br23/board_ac635n_demo.c \\\n',
      f'\t{UP}/apps/hid/board/br23/board_ac635n_demo.c \\\n' +
      ''.join(f'\t{UP}/{s} \\\n' for s in sorted(srcs)),
      f'gamepad sources ({len(srcs)} files)')

# --- reproducible builds (docs/plan.md 6.4) ------------------------------------
# The SDK logs __DATE__ / __TIME__ (apps/hid/app_main.c, cpu/br23/setup.c):
# the only thing that made two builds of one tree differ. Fixed values (the
# usual reproducible-builds epoch); the commit is in INFO (GP_FW_VERSION).
# clang 4 has no SOURCE_DATE_EPOCH; redefining the builtins only warns (-w).
patch(MK, '\t-DSUPPORT_MS_EXTENSIONS \\\n',
      '\t-DSUPPORT_MS_EXTENSIONS \\\n'
      '\t-D__DATE__=\'"Jan  1 1970"\' -D__TIME__=\'"00:00:00"\' \\\n',
      'fixed __DATE__ / __TIME__')

# --- board selection: board_<name>_cfg.h is found on the include path ---------
cfg = f'{SDK}/{BR23}/board_config.h'
s = open(cfg, encoding='utf-8').read()
define = f'CONFIG_BOARD_{BOARD.upper()}'
if define not in s:
    s2 = re.sub(r'^#define CONFIG_BOARD_AC635N_DEMO$',
                f'// #define CONFIG_BOARD_AC635N_DEMO\n#define {define}', s, flags=re.M)
    if s2 == s:
        sys.exit(f'error: {BR23}/board_config.h: demo board define not found')
    s2 = s2.replace('#include "board_ac635n_demo_cfg.h"',
                    f'#include "board_ac635n_demo_cfg.h"\n#include "board_{BOARD}_cfg.h"')
    open(cfg, 'w', encoding='utf-8').write(s2)
    print(f'  {BR23}/board_config.h: select {define}')

# --- the receiver's crash-loop guard needs RAM that startup doesn't clear:
#     .non_volatile_ram is merged into the zeroed .bss, so a NOLOAD section
#     of its own after it (the heap starts after the overlays, after this) --
patch('cpu/br23/sdk_ld.c',
      '\toverlay_begin = .;\n    #include "app_overlay.ld"\n',
      '#ifdef CONFIG_DONGLE_FW   /* receiver: RAM startup does not clear (firmware/sdk/patch_sdk.py) */\n'
      '    .dg_noinit (NOLOAD) : ALIGN(4)\n'
      '    {\n'
      '        *(.dg_noinit)\n'
      '    } > ram0\n'
      '#endif\n'
      '\toverlay_begin = .;\n    #include "app_overlay.ld"\n',
      'no-init RAM section for the receiver')

# --- the receiver's crash-loop guard runs first in setup_arch(), before the
#     watchdog, clock and voltage set-up: a reset loop there (the likely
#     hang of the first stage-A image) then ends in the bootloader ---------
patch('cpu/br23/setup.c',
      'void setup_arch()\n{\n    memory_init();\n',
      'void setup_arch()\n{\n'
      '#ifdef CONFIG_DONGLE_FW   /* receiver: firmware/sdk/patch_sdk.py */\n'
      '    extern void dg_guard_boot(void);\n'
      '    dg_guard_boot();\n'
      '#endif\n'
      '    memory_init();\n',
      'receiver guard first in setup_arch')

# --- receiver: an LED checkpoint after the clock set-up (stage A bring-up) --
patch('cpu/br23/setup.c',
      '    clk_early_init(TCFG_CLOCK_SYS_SRC, TCFG_CLOCK_OSC_HZ, TCFG_CLOCK_SYS_HZ);\n',
      '    clk_early_init(TCFG_CLOCK_SYS_SRC, TCFG_CLOCK_OSC_HZ, TCFG_CLOCK_SYS_HZ);\n'
      '#ifdef CONFIG_DONGLE_FW   /* receiver: firmware/sdk/patch_sdk.py */\n'
      '    extern void dg_checkpoint(int n);\n'
      '    dg_checkpoint(2);\n'
      '#endif\n',
      'receiver LED checkpoint after the clock set-up')

# --- the receiver's container puts app.bin at offset 0xC0 of its app area
#     (the pad's: 0x120), so the receiver app runs at 0x1E000C0: link it
#     there. (Linked at 0x1E00120 it ran 0x60 low: relative branches worked,
#     absolute ones, the startup's jump to main among them, did not, and
#     calls into the ROM landed 0x60 low; docs/dongle.md.) ----------------
patch('cpu/br23/sdk_ld.c',
      '\tcode0(rx)    \t  : ORIGIN =  0x1E00120,    LENGTH = CONFIG_FLASH_SIZE\n',
      '#ifdef CONFIG_DONGLE_FW   /* receiver: app.bin at app area + 0xC0 (firmware/sdk/patch_sdk.py) */\n'
      '\tcode0(rx)    \t  : ORIGIN =  0x1E000C0,    LENGTH = CONFIG_FLASH_SIZE\n'
      '#else\n'
      '\tcode0(rx)    \t  : ORIGIN =  0x1E00120,    LENGTH = CONFIG_FLASH_SIZE\n'
      '#endif\n',
      'receiver app linked at 0x1E000C0')

# --- receiver bring-up: marks before main() (boards/dongle: dg_early_mark).
#     The startup (prebuilt) calls app_load_common_code() after its .bss /
#     .data / cache set-up, then update_result_get(), then main() ---------
patch('cpu/br23/setup.c',
      'void app_load_common_code()\n{\n',
      'void app_load_common_code()\n{\n'
      '#ifdef CONFIG_DONGLE_FW   /* receiver: firmware/sdk/patch_sdk.py */\n'
      '    extern void dg_early_mark(int k);\n'
      '    dg_early_mark(0);\n'
      '#endif\n',
      'receiver mark 0 (startup done)')
patch('apps/common/update/update.c',
      'u16 update_result_get(void)\n{\n    u16 ret = UPDATA_NON;\n',
      '#ifdef CONFIG_DONGLE_FW   /* receiver: firmware/sdk/patch_sdk.py */\n'
      'extern void dg_early_mark(int k);\n'
      '#endif\n'
      'u16 update_result_get(void)\n{\n    u16 ret = UPDATA_NON;\n'
      '#ifdef CONFIG_DONGLE_FW\n'
      '    dg_early_mark(1);\n'
      '#endif\n',
      'receiver mark 1 (update_result_get)')
patch('apps/common/update/update.c',
      '        memset(p, 0x00, sizeof(UPDATA_PARM));\n    }\n\n    return ret;\n}\n',
      '        memset(p, 0x00, sizeof(UPDATA_PARM));\n    }\n\n'
      '#ifdef CONFIG_DONGLE_FW\n'
      '    dg_early_mark(2);\n'
      '#endif\n'
      '    return ret;\n}\n',
      'receiver mark 2 (update_result_get done)')

# --- the receiver searches for a pad to pair (inquiry): apps/hid builds the
#     library with inquiry off (spp_and_le turns it on with its emitter) ----
patch('apps/hid/config/lib_profile_config.c',
      'const u8 hci_inquiry_support = 0;\n',
      '#ifdef CONFIG_DONGLE_FW   /* receiver: pairing searches (firmware/sdk/patch_sdk.py) */\n'
      'const u8 hci_inquiry_support = 1;\n'
      '#else\n'
      'const u8 hci_inquiry_support = 0;\n'
      '#endif\n',
      'receiver: host inquiry on')
patch('apps/hid/config/lib_btctrler_config.c',
      'const int CONFIG_BREDR_INQUIRY   =  0;\n',
      '#ifdef CONFIG_DONGLE_FW   /* receiver: pairing searches (firmware/sdk/patch_sdk.py) */\n'
      'const int CONFIG_BREDR_INQUIRY   =  1;\n'
      '#else\n'
      'const int CONFIG_BREDR_INQUIRY   =  0;\n'
      '#endif\n',
      'receiver: controller inquiry on')

# --- the gamepad app replaces the keyboard example ---------------------------
patch('apps/hid/examples/keyboard/app_keyboard.c',
      '#if(CONFIG_APP_KEYBOARD)\n',
      '#if(CONFIG_APP_KEYBOARD) && !defined(CONFIG_GAMEPAD_FW)   /* gamepad: apps/gamepad/src/gp_main.c */\n',
      'keyboard example off')

# --- Bluetooth identity: the board's Device ID (SDL picks its SInput driver
#     by VID/PID), a gamepad HID record ------------------------------------------
CFG = 'apps/hid/config/lib_profile_config.c'
patch(CFG,
      '#define  PNP_VID_SOURCE   0x02\n'
      '#define  PNP_VID          0x05ac //0x05d6\n'
      '#define  PNP_PID          0x022C //\n'
      '#define  PNP_PID_VERSION  0x011b //1.1.11\n',
      '#ifdef CONFIG_GAMEPAD_FW   /* gamepad: the board\'s identity (firmware/sdk/patch_sdk.py) */\n'
      '#define  PNP_VID_SOURCE   0x02          /* USB-IF */\n'
      '#define  PNP_VID          GP_PNP_VID\n'
      '#define  PNP_PID          GP_PNP_PID\n'
      '#define  PNP_PID_VERSION  GP_PNP_VERSION\n'
      '#define  GP_HID_SUBCLASS  0x08          /* gamepad */\n'
      '#else\n'
      '#define  PNP_VID_SOURCE   0x02\n'
      '#define  PNP_VID          0x05ac //0x05d6\n'
      '#define  PNP_PID          0x022C //\n'
      '#define  PNP_PID_VERSION  0x011b //1.1.11\n'
      '#define  GP_HID_SUBCLASS  0x80\n'
      '#endif\n',
      'Device ID from the board')
patch(CFG, '    .sub_class           = 0x80,\n',
      '    .sub_class           = GP_HID_SUBCLASS,\n',
      'HID subclass')
patch(CFG,
      '    .service_name        = "JL_HID",\n'
      '    .service_description = "hid key",\n'
      '    .provide_name        = "JIELI",\n',
      '#ifdef CONFIG_GAMEPAD_FW\n'
      '    .service_name        = GP_PRODUCT_NAME,\n'
      '    .service_description = "Gamepad",\n'
      '    .provide_name        = GP_VENDOR_NAME,\n'
      '#else\n'
      '    .service_name        = "JL_HID",\n'
      '    .service_description = "hid key",\n'
      '    .provide_name        = "JIELI",\n'
      '#endif\n',
      'HID service names')

# --- the Bluetooth identity follows the input mode (Steam: SInput, Xbox: an
#     Xbox One S pad): the Device ID record and the HID record's info
#     become writable, and gp_sdp_set_identity() sets them before the stack
#     starts (core/gp_bt.c) -------------------------------------------------
patch(CFG, 'static const u8 sdp_pnp_service_data_hid[] = {\n',
      '#ifdef CONFIG_GAMEPAD_FW   /* gamepad: set at start-up by gp_sdp_set_identity */\n'
      'static u8 sdp_pnp_service_data_hid[] = {\n'
      '#else\n'
      'static const u8 sdp_pnp_service_data_hid[] = {\n'
      '#endif\n',
      'writable Device ID record')
patch(CFG, 'static const hid_sdp_info_t hid_sdp_info_config = {\n',
      '#ifdef CONFIG_GAMEPAD_FW   /* gamepad: set at start-up by gp_sdp_set_identity */\n'
      'static hid_sdp_info_t hid_sdp_info_config = {\n'
      '#else\n'
      'static const hid_sdp_info_t hid_sdp_info_config = {\n'
      '#endif\n',
      'writable HID record info')
patch(CFG, 'void hid_sdp_init(const u8 *hid_descriptor, u16 size)\n{\n',
      '#ifdef CONFIG_GAMEPAD_FW   /* gamepad: firmware/sdk/patch_sdk.py */\n'
      'void gp_sdp_set_identity(u16 vid, u16 pid, u16 version, const char *name)\n'
      '{\n'
      '    hid_sdp_info_config.vid_private = vid;\n'
      '    hid_sdp_info_config.pid_private = pid;\n'
      '    hid_sdp_info_config.ver_private = version;\n'
      '    hid_sdp_info_config.service_name = (char *)name;\n'
      '    /* Device ID record: the u16 after attributes 0x0201 VID, 0x0202 PID, 0x0203 version */\n'
      '    u8 *p = sdp_pnp_service_data_hid;\n'
      '    for (int i = 0; i + 5 < (int)sizeof(sdp_pnp_service_data_hid); i++) {\n'
      '        if (p[i] == 0x09 && p[i + 1] == 0x02 && p[i + 2] >= 1 && p[i + 2] <= 3 && p[i + 3] == 0x09) {\n'
      '            u16 v = p[i + 2] == 1 ? vid : p[i + 2] == 2 ? pid : version;\n'
      '            p[i + 4] = v >> 8;\n'
      '            p[i + 5] = v & 0xff;\n'
      '        }\n'
      '    }\n'
      '}\n'
      '#endif\n'
      'void hid_sdp_init(const u8 *hid_descriptor, u16 size)\n{\n',
      'gp_sdp_set_identity')

# --- the HID service record is built into a fixed 512-byte buffer: the
#     Xbox One S descriptor (334 B) plus the record's other attributes
#     overflow it (Xbox mode boot-looped). Room for 768 ---------------------
patch(CFG, 'u8 sdp_make_hid_service_data[0x200];\n',
      '#ifdef CONFIG_GAMEPAD_FW   /* gamepad: the Xbox One S descriptor is 334 B */\n'
      'u8 sdp_make_hid_service_data[0x300];\n'
      '#else\n'
      'u8 sdp_make_hid_service_data[0x200];\n'
      '#endif\n',
      'HID service record buffer 768 B')

# --- SInput reports are 64 B, DS4's Bluetooth report 78; the SDK's classic
#     HID path stops at 14 ---------------------------------------------------
HID = 'apps/hid/modules/bt/edr_hid_user.c'
patch(HID, '#define HID_SEND_MAX_SIZE   (16) //描述符数据包的长度\n',
      '#ifdef CONFIG_GAMEPAD_FW\n'
      '#define HID_SEND_MAX_SIZE   (2 + 80)   /* gamepad: type + ID + a report (SInput 63 B, DS4 77 B) */\n'
      '#else\n'
      '#define HID_SEND_MAX_SIZE   (16) //描述符数据包的长度\n'
      '#endif\n',
      'HID report size 64')
patch(HID, '#define HID_TMP_BUFSIZE  (64*2)\n',
      '#ifdef CONFIG_GAMEPAD_FW\n'
      '#define HID_TMP_BUFSIZE  ((2 + HID_SEND_MAX_SIZE) * 4)   /* gamepad: room for 4 reports */\n'
      '#else\n'
      '#define HID_TMP_BUFSIZE  (64*2)\n'
      '#endif\n',
      'HID send queue of 4 reports')

# --- a gamepad sends all the time: no device-initiated sniff (the anchor
#     profile capped the link at one report per 10 ms) ----------------------
patch('apps/hid/modules/bt/app_comm_edr.c',
      '#define SNIFF_ENABLE                  1 //是否允许主动进入sniff\n',
      '#ifdef CONFIG_GAMEPAD_FW\n'
      '#define SNIFF_ENABLE                  0   /* gamepad: reports continuously */\n'
      '#else\n'
      '#define SNIFF_ENABLE                  1 //是否允许主动进入sniff\n'
      '#endif\n',
      'no device-initiated sniff')

# --- the SDK's battery logic (warning, power-off, percent) reads the chip's
#     VBAT, which on the C2 is not the battery (measured: a flat 4548 mV,
#     unchanged by charging). Use the board's battery pin and divider. Below
#     1 V means no sample yet (a new channel's value starts at 1; the board
#     registers it right after adc_init): report full, not "empty, power
#     off". ------------------------------------------------------------------
patch('apps/hid/modules/power/app_power_manage.c',
      '    return (adc_get_voltage(AD_CH_VBAT) * 4 / 10);\n',
      '#ifdef CONFIG_GAMEPAD_FW   /* gamepad: the board\'s battery pin, not the chip\'s VBAT */\n'
      '    u32 mv = adc_get_voltage(GP_BATTERY_ADC_CH) * GP_BATTERY_DIVIDER;\n'
      '    return mv >= 1000 ? mv / 10 : 420;   /* < 1 V: not sampled yet (a new channel starts at 1) */\n'
      '#else\n'
      '    return (adc_get_voltage(AD_CH_VBAT) * 4 / 10);\n'
      '#endif\n',
      'battery level from the board\'s pin')

# --- USB device descriptor and strings: core/gp_usb.c provides them, so the
#     IDs follow the mode (the gamepad's VID/PID, or the debug log's
#     3537:01AB in charging mode). The SDK's fixed ones are renamed away (its
#     tables stay: get_device_info_to_ota reads them). ------------------------
DESC = 'apps/common/device/usb/device/descriptor.c'
patch(DESC,
      'void get_device_descriptor(u8 *ptr)\n',
      '#ifdef CONFIG_GAMEPAD_FW   /* gamepad: core/gp_usb.c provides these (firmware/sdk/patch_sdk.py) */\n'
      '#define get_device_descriptor   sdk_get_device_descriptor\n'
      '#define get_manufacture_str     sdk_get_manufacture_str\n'
      '#define get_product_str         sdk_get_product_str\n'
      '#endif\n'
      'void get_device_descriptor(u8 *ptr)\n',
      'USB descriptor and strings from the firmware')

# --- the USB endpoints' DMA pool holds 64 B for HID: SInput's IN endpoint.
#     DS4 mode has an interrupt OUT endpoint too (hid-playstation sends its
#     rumble and lightbar there, with no SET_REPORT fallback): 64 more, on
#     the pad (core/gp_usb.c) and the receiver (boards/dongle/dg_usb.c) -----
patch('apps/common/device/usb/usb_config.c',
      '#if TCFG_USB_SLAVE_HID_ENABLE\n#define     HID_DMA_SIZE    64\n',
      '#if TCFG_USB_SLAVE_HID_ENABLE\n'
      '#ifdef CONFIG_GAMEPAD_FW   /* the pad, the receiver: DS4 IN 64 + OUT 32 (firmware/sdk/patch_sdk.py) */\n'
      '#define     HID_DMA_SIZE    (64 + 64)\n'
      '#else\n'
      '#define     HID_DMA_SIZE    64\n'
      '#endif\n',
      'USB DMA pool: room for the DS4 OUT endpoint')

# --- device-level control requests the SDK doesn't know (Microsoft OS
#     descriptors in XInput mode): core/gp_usb.c sees them first ----------
# --- CDC (the debug log): don't drop the packet in flight on every open ------
# Linux sends SET_CONTROL_LINE_STATE on *every* open of the tty (DTR raised
# again), and the SDK re-initialised both bulk endpoints on each one: the
# log packet waiting on IN was lost each time a second program opened the
# port (c2log --boot, a command), 2026-11-13. Now only the first one after a
# bus reset sets the endpoints up (the reset handler already does on this
# chip), and a reset clears the "port open" bits, so the log waits for a
# new open after re-enumeration.
CDC = 'apps/common/device/usb/device/cdc.c'
patch(CDC, '''            usb_set_setup_phase(usb_device, USB_EP0_STAGE_SETUP);
            cdc_endpoint_init(usb_device, (ctrl_req->wIndex & USB_RECIP_MASK));
''', '''            usb_set_setup_phase(usb_device, USB_EP0_STAGE_SETUP);
            if (!cdc_eps_ready) {   /* firmware/sdk/patch_sdk.py: not on every open */
                cdc_eps_ready = 1;
                cdc_endpoint_init(usb_device, (ctrl_req->wIndex & USB_RECIP_MASK));
            }
''', 'CDC: endpoints set up once per reset, not on every open')
patch(CDC, '''static void cdc_endpoint_init(struct usb_device_t *usb_device, u32 itf);
''', '''static void cdc_endpoint_init(struct usb_device_t *usb_device, u32 itf);
static u8 cdc_eps_ready;    /* firmware/sdk/patch_sdk.py */
''', 'CDC: endpoint flag')
patch(CDC, '''    log_error("%s()", __func__);
    const usb_dev usb_id = usb_device2id(usb_device);
#if USB_ROOT2''', '''    log_error("%s()", __func__);
    const usb_dev usb_id = usb_device2id(usb_device);
    cdc_eps_ready = 0;      /* firmware/sdk/patch_sdk.py */
    if (cdc_hdl) {
        cdc_hdl->bmTransceiver = 0;
    }
#if USB_ROOT2''', 'CDC: a bus reset closes the port')

patch('apps/common/device/usb/device/user_setup.c',
      '    u32 ret = 0;\n'
      '    u32 recip = request->bRequestType & USB_RECIP_MASK;\n',
      '    u32 ret = 0;\n'
      '#ifdef CONFIG_GAMEPAD_FW   /* gamepad: firmware/sdk/patch_sdk.py */\n'
      '    extern u32 gp_usb_setup(struct usb_device_t *usb_device, struct usb_ctrlrequest *req);\n'
      '    if (gp_usb_setup(usb_device, request)) {\n'
      '        return 1;\n'
      '    }\n'
      '#endif\n'
      '    u32 recip = request->bRequestType & USB_RECIP_MASK;\n',
      'firmware sees device requests first')

# --- UART2 (the stick chip, drivers/hjc_stick.c): the RX-threshold interrupt
#     adds frame_length to the ring's write index, ignoring the hardware's
#     byte count it has just read. When the interrupt runs late and two
#     thresholds went by, the index falls frame_length behind for good, and
#     as the lag drifts round the ring the reader meets the DMA: bursts of
#     CRC errors and missing frames (debug log, 2026-11-13). Use HRXCNT,
#     falling back to frame_length if it looks wrong, and count the times
#     they differ (gp_uart2_slips). ------------------------------------------
patch('cpu/br23/uart_dev.c',
      '        rx_len = JL_UART2->HRXCNT;             //读当前串口接收数据的个数\n'
      '        uart2.kfifo.buf_in += uart2.frame_length; //每满32字节则产生一次中断\n',
      '        rx_len = JL_UART2->HRXCNT;             //读当前串口接收数据的个数\n'
      '#ifdef CONFIG_GAMEPAD_FW   /* gamepad: the real count (firmware/sdk/patch_sdk.py) */\n'
      '        {\n'
      '            extern volatile u32 gp_uart2_slips, gp_uart2_bogus;\n'
      '            if (rx_len >= uart2.frame_length && rx_len < uart2.kfifo.buf_size) {\n'
      '                if (rx_len != uart2.frame_length) {\n'
      '                    gp_uart2_slips++;\n'
      '                }\n'
      '                uart2.kfifo.buf_in += rx_len;\n'
      '            } else {\n'
      '                gp_uart2_bogus++;\n'
      '                uart2.kfifo.buf_in += uart2.frame_length;\n'
      '            }\n'
      '        }\n'
      '#else\n'
      '        uart2.kfifo.buf_in += uart2.frame_length; //每满32字节则产生一次中断\n'
      '#endif\n',
      'UART2 RX index from the hardware count')

# --- GET_REPORT on the HID control channel: the library asks this hook
#     first (0: it answers itself). DS4 mode's feature reports come from the
#     pad's firmware (core/gp_bt.c, gp_bt_get_report; not the receiver's) ----
patch(HID,
      'int hid_ctrl_data_parse_hook(u16 channel, const u8 *packet, int size, int *respond_buf_ptr)\n'
      '{\n',
      'int hid_ctrl_data_parse_hook(u16 channel, const u8 *packet, int size, int *respond_buf_ptr)\n'
      '{\n'
      '#if defined(CONFIG_GAMEPAD_FW) && !defined(CONFIG_DONGLE_FW)   /* the pad: firmware/sdk/patch_sdk.py */\n'
      '    if ((packet[0] & 0xf0) == HID_GET_REPORT) {\n'
      '        extern int gp_bt_get_report(u16 cid, const u8 *packet, int size);\n'
      '        if (gp_bt_get_report(channel, packet, size)) {\n'
      '            return 0;   /* sent: longer than the library\'s 16-byte buffer */\n'
      '        }\n'
      '    }\n'
      '#endif\n',
      'GET_REPORT from the firmware')

# --- a HID send that never completes left bt_send_busy set for good: the
#     pad stayed connected with no input after a 19 s BT-stack stall
#     (2026-11-13). Let the firmware clear it. -------------------------------
patch(HID,
      'int edr_hid_is_busy(void)\n{\n    return bt_send_busy;\n}\n',
      'int edr_hid_is_busy(void)\n{\n    return bt_send_busy;\n}\n'
      '#ifdef CONFIG_GAMEPAD_FW   /* gamepad: firmware/sdk/patch_sdk.py */\n'
      'void edr_hid_send_unstick(void)\n{\n    bt_send_busy = 0;\n}\n'
      '#endif\n',
      'edr_hid_send_unstick')

# --- keep the result of each HID send for the debug log: the SDK drops it
#     (it only clears bt_send_busy). user_send_cmd_prepare's HID path returns
#     30 (TX pool < 256 B free), 4 (the last report not yet sent), 3, 2 (no
#     HID connection), 1. -----------------------------------------------------
patch(HID,
      '    if (ret) {\n        log_info("hid send fail!!! %d\\n", ret);\n    }\n    return ret;\n',
      '    if (ret) {\n        log_info("hid send fail!!! %d\\n", ret);\n'
      '#ifdef CONFIG_GAMEPAD_FW   /* gamepad: firmware/sdk/patch_sdk.py */\n'
      '        extern volatile int gp_hid_send_err;\n'
      '        extern volatile u32 gp_hid_send_errs;\n'
      '        gp_hid_send_err = ret;\n'
      '        gp_hid_send_errs++;\n'
      '#endif\n'
      '    }\n    return ret;\n',
      'keep HID send results')
