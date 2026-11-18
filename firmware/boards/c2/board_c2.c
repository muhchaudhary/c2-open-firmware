/* board_c2.c: the GameSir Cyclone 2 as an SDK board (chip set-up, power,
 * wake-up, soft-off). Derived from the SDK's board_ac635n_demo.c; the C2
 * changes are commented with "C2". Pins and values: c2_pins.h and
 * c2_power.h (decompiled from the stock app v3.56). The C2 as a gamepad is
 * gamepad_c2.c. */
#include "app_config.h"

#ifdef CONFIG_BOARD_C2

#include "system/includes.h"
#include "asm/charge.h"
#include "device/key_driver.h"
#include "asm/power/p33.h"
#include "asm/iic_soft.h"
#include "usb/otg.h"
#include "rtc_alarm.h"
#include "asm/power/power_port.h"
#include "c2_pins.h"
#include "c2_power.h"
#include "gamepad.h"
#include "ws2812_spi.h"

#ifdef CONFIG_LITE_AUDIO
#include "media/includes.h"
#endif/*CONFIG_LITE_AUDIO*/

#define LOG_TAG_CONST       BOARD
#define LOG_TAG             "[BOARD]"
#define LOG_ERROR_ENABLE
#define LOG_DEBUG_ENABLE
#define LOG_INFO_ENABLE
/* #define LOG_DUMP_ENABLE */
#define LOG_CLI_ENABLE
#include "debug.h"

void board_power_init(void);
const struct soft_iic_config soft_iic_cfg[] = {
    //iic0 data
    {
        .scl = C2_IIC_SCL,                     /* C2: the IMU bus (PB04/PB06); unused in phase 3 */
        .sda = C2_IIC_SDA,
        .delay = 50,                //软件IIC延时参数，影响通讯时钟频率
        .io_pu = 1,                                     //是否打开上拉电阻，如果外部电路没有焊接上拉电阻需要置1
    },
};

/************************** LOW POWER config ****************************/
struct low_power_param power_param = {     /* C2: not const, vddiom_lev is set in board_power_init */
    .config         = TCFG_LOWPOWER_LOWPOWER_SEL,          //0：sniff时芯片不进入低功耗  1：sniff时芯片进入powerdown
    .btosc_hz         = TCFG_CLOCK_OSC_HZ,                   //外接晶振频率
    .delay_us       = TCFG_CLOCK_SYS_HZ / 1000000L,        //提供给低功耗模块的延时(不需要需修改)
    .btosc_disable  = TCFG_LOWPOWER_BTOSC_DISABLE,         //进入低功耗时BTOSC是否保持
    .vddiom_lev     = TCFG_LOWPOWER_VDDIOM_LEVEL,          //强VDDIO等级,可选：2.0V  2.2V  2.4V  2.6V  2.8V  3.0V  3.2V  3.6V
    .vddiow_lev     = TCFG_LOWPOWER_VDDIOW_LEVEL,          //弱VDDIO等级,可选：2.1V  2.4V  2.8V  3.2V
    .osc_type       = OSC_TYPE_LRC,
    .dcdc_port      = TCFG_DCDC_PORT_SEL,
#if TCFG_RTC_ALARM_ENABLE
    .rtc_clk    	= CLK_SEL_32K,
#endif
};


/************************** KEY MSG****************************/
/*各个按键的消息设置，如果USER_CFG中设置了USE_CONFIG_KEY_SETTING为1，则会从配置文件读取对应的配置来填充改结构体*/
u8 key_table[KEY_NUM_MAX][KEY_EVENT_MAX] = {
    // SHORT           LONG              HOLD              UP              DOUBLE           TRIPLE
};


// *INDENT-OFF*
/************************** UART config****************************/
#if TCFG_UART0_ENABLE
UART0_PLATFORM_DATA_BEGIN(uart0_data)
    .tx_pin = TCFG_UART0_TX_PORT,                             //串口打印TX引脚选择
    .rx_pin = TCFG_UART0_RX_PORT,                             //串口打印RX引脚选择
    .baudrate = TCFG_UART0_BAUDRATE,                          //串口波特率

    .flags = UART_DEBUG,                                      //串口用来打印需要把改参数设置为UART_DEBUG
UART0_PLATFORM_DATA_END()
#endif //TCFG_UART0_ENABLE


/************************** CHARGE config****************************/
#if TCFG_CHARGE_ENABLE
CHARGE_PLATFORM_DATA_BEGIN(charge_data)
    .charge_en              = TCFG_CHARGE_ENABLE,              //内置充电使能
    .charge_poweron_en      = TCFG_CHARGE_POWERON_ENABLE,      //是否支持充电开机
    .charge_full_V          = TCFG_CHARGE_FULL_V,              //充电截止电压
    .charge_full_mA			= TCFG_CHARGE_FULL_MA,             //充电截止电流
    .charge_mA				= TCFG_CHARGE_MA,                  //充电电流
/*ldo5v拔出过滤值，过滤时间 = (filter*2 + 20)ms,ldoin<0.6V且时间大于过滤时间才认为拔出
 对于充满直接从5V掉到0V的充电仓，该值必须设置成0，对于充满由5V先掉到0V之后再升压到xV的
 充电仓，需要根据实际情况设置该值大小*/
	.ldo5v_off_filter		= 100,
/*ldo5v的10k下拉电阻使能,若充电舱需要更大的负载才能检测到插入时，请将该变量置1,默认值设置为1
  对于充电舱需要按键升压,且维持电压是从充电舱经过上拉电阻到充电口的舱，请将该值改为0*/
	.ldo5v_pulldown_en		= 0,
CHARGE_PLATFORM_DATA_END()
#endif//TCFG_CHARGE_ENABLE

    /************************** AD KEY ****************************/
#if TCFG_ADKEY_ENABLE
const struct adkey_platform_data adkey_data = {
    .enable = TCFG_ADKEY_ENABLE,                              //AD按键使能
    .adkey_pin = TCFG_ADKEY_PORT,                             //AD按键对应引脚
    .ad_channel = TCFG_ADKEY_AD_CHANNEL,                      //AD通道值
    .extern_up_en = TCFG_ADKEY_EXTERN_UP_ENABLE,              //是否使用外接上拉电阻
    .ad_value = {                                             //根据电阻算出来的电压值
        TCFG_ADKEY_VOLTAGE0,
        TCFG_ADKEY_VOLTAGE1,
        TCFG_ADKEY_VOLTAGE2,
        TCFG_ADKEY_VOLTAGE3,
        TCFG_ADKEY_VOLTAGE4,
        TCFG_ADKEY_VOLTAGE5,
        TCFG_ADKEY_VOLTAGE6,
        TCFG_ADKEY_VOLTAGE7,
        TCFG_ADKEY_VOLTAGE8,
        TCFG_ADKEY_VOLTAGE9,
    },
    .key_value = {                                             //AD按键各个按键的键值
        TCFG_ADKEY_VALUE0,
        TCFG_ADKEY_VALUE1,
        TCFG_ADKEY_VALUE2,
        TCFG_ADKEY_VALUE3,
        TCFG_ADKEY_VALUE4,
        TCFG_ADKEY_VALUE5,
        TCFG_ADKEY_VALUE6,
        TCFG_ADKEY_VALUE7,
        TCFG_ADKEY_VALUE8,
        TCFG_ADKEY_VALUE9,
    },
};
#endif

#if TCFG_IRKEY_ENABLE
const struct irkey_platform_data irkey_data = {
	    .enable = TCFG_IRKEY_ENABLE,                              //IR按键使能
	    .port = TCFG_IRKEY_PORT,                                       //IR按键口
};
#endif

/************************** IO KEY ****************************/
#if TCFG_IOKEY_ENABLE
const struct iokey_port iokey_list[] = {
	{
		.connect_way = TCFG_IOKEY_POWER_CONNECT_WAY,          //IO按键的连接方式
		.key_type.one_io.port = TCFG_IOKEY_POWER_ONE_PORT,    //IO按键对应的引脚
		.key_value = TCFG_IOKEY_POWER_ONE_PORT_VALUE,         //按键值
	},

	/* { */
		/* .connect_way = TCFG_IOKEY_PREV_CONNECT_WAY, */
		/* .key_type.one_io.port = TCFG_IOKEY_PREV_ONE_PORT, */
		/* .key_value = TCFG_IOKEY_PREV_ONE_PORT_VALUE, */
	/* }, */

	/* { */
		/* .connect_way = TCFG_IOKEY_NEXT_CONNECT_WAY, */
		/* .key_type.one_io.port = TCFG_IOKEY_NEXT_ONE_PORT, */
		/* .key_value = TCFG_IOKEY_NEXT_ONE_PORT_VALUE, */
	/* }, */
};
const struct iokey_platform_data iokey_data = {
	.enable = TCFG_IOKEY_ENABLE,                              //是否使能IO按键
	.num = ARRAY_SIZE(iokey_list),                            //IO按键的个数
	.port = iokey_list,                                       //IO按键参数表
};

#if MULT_KEY_ENABLE
//组合按键消息映射表
//配置注意事项:单个按键按键值需要按照顺序编号,如power:0, prev:1, next:2
//bit_value = BIT(0) | BIT(1) 指按键值为0和按键值为1的两个按键被同时按下,
//remap_value = 3指当这两个按键被同时按下后重新映射的按键值;
const struct key_remap iokey_remap_table[] = {
	{.bit_value = BIT(0) | BIT(1), .remap_value = 3},
	{.bit_value = BIT(0) | BIT(2), .remap_value = 4},
	{.bit_value = BIT(1) | BIT(2), .remap_value = 5},
};

const struct key_remap_data iokey_remap_data = {
	.remap_num = ARRAY_SIZE(iokey_remap_table),
	.table = iokey_remap_table,
};
#endif

#endif

#if TCFG_RTC_ALARM_ENABLE
const struct sys_time def_sys_time = {  //初始一下当前时间
    .year = 2020,
    .month = 1,
    .day = 1,
    .hour = 0,
    .min = 0,
    .sec = 0,
};
const struct sys_time def_alarm = {     //初始一下目标时间，即闹钟时间
    .year = 2020,
    .month = 1,
    .day = 1,
    .hour = 0,
    .min = 0,
    .sec = 7,
};

extern void alarm_isr_user_cbfun(u8 index);
RTC_DEV_PLATFORM_DATA_BEGIN(rtc_data)
	.default_sys_time = &def_sys_time,
	.default_alarm = &def_alarm,
    /* .cbfun = NULL,                      //闹钟中断的回调函数,用户自行定义 */
    .cbfun = alarm_isr_user_cbfun,
RTC_DEV_PLATFORM_DATA_END()
#endif

/************************** otg data****************************/
#if TCFG_OTG_MODE
struct otg_dev_data otg_data = {
    .usb_dev_en = TCFG_OTG_USB_DEV_EN,
	.slave_online_cnt = TCFG_OTG_SLAVE_ONLINE_CNT,
	.slave_offline_cnt = TCFG_OTG_SLAVE_OFFLINE_CNT,
	.host_online_cnt = TCFG_OTG_HOST_ONLINE_CNT,
	.host_offline_cnt = TCFG_OTG_HOST_OFFLINE_CNT,
	.detect_mode = TCFG_OTG_MODE,
	.detect_time_interval = TCFG_OTG_DET_INTERVAL,
};
#endif
REGISTER_DEVICES(device_table) = {
#if TCFG_OTG_MODE
    { "otg",     &usb_dev_ops, (void *) &otg_data},
#endif
};
void debug_uart_init(const struct uart_platform_data *data)
{
#if TCFG_UART0_ENABLE
    if (data) {
        uart_init(data);
    } else {
        uart_init(&uart0_data);
    }
#endif
}

/*其他封装板级，移该函数*/
u8 get_power_on_status(void)
{
#if TCFG_IOKEY_ENABLE
    struct iokey_port *power_io_list = NULL;
    power_io_list = iokey_data.port;

    if (iokey_data.enable) {
        if (gpio_read(power_io_list->key_type.one_io.port) == power_io_list->connect_way){
            return 1;
        }
    }
#endif

#if TCFG_ADKEY_ENABLE
    if (adkey_data.enable) {
        return 1;
    }
#endif

#if defined(TCFG_LP_TOUCH_KEY_ENABLE) && TCFG_LP_TOUCH_KEY_ENABLE
    return lp_touch_key_power_on_status();
#endif

    return 0;
}


static void board_devices_init(void)
{
#if TCFG_PWMLED_ENABLE
    pwm_led_init(&pwm_led_data);
#endif

#if (TCFG_IOKEY_ENABLE || TCFG_ADKEY_ENABLE || TCFG_IRKEY_ENABLE || TCFG_TOUCH_KEY_ENABLE)
	key_driver_init();
#endif

#if TCFG_RTC_ALARM_ENABLE
    alarm_init(&rtc_data);
#endif
}

extern void set_dcdc_ctl_port(u8 port);
extern void cfg_file_parse(u8 idx);
/* C2 pin setup, as stock's "input port init" 0x1E3421A (c2_pins.h). */
static const u8 c2_buttons[] = {
    C2_KEY_UP, C2_KEY_DOWN, C2_KEY_LEFT, C2_KEY_RIGHT, C2_KEY_L1, C2_KEY_R1,
    C2_KEY_L3, C2_KEY_R3, C2_KEY_PADDLE_A, C2_KEY_PADDLE_B, C2_KEY_SOUTH,
    C2_KEY_EAST, C2_KEY_WEST, C2_KEY_NORTH, C2_KEY_HOME, C2_KEY_SHARE,
    C2_KEY_OPTIONS, C2_KEY_CAPTURE, C2_KEY_BIT22, IO_PORTB_02,
};
static const u8 c2_analog[] = { C2_LT, C2_RT, C2_AN_PA01, C2_VBAT_DIV, C2_AN_PA12 };

static void c2_out(u8 pin, u8 level)
{
    gpio_set_pull_up(pin, 0);
    gpio_set_pull_down(pin, 0);
    gpio_set_die(pin, 1);
    gpio_direction_output(pin, level);
}

static void c2_gpio_init(void)
{
    for (int i = 0; i < ARRAY_SIZE(c2_buttons); i++) {
        gpio_set_pull_up(c2_buttons[i], 1);
        gpio_set_pull_down(c2_buttons[i], 0);
        gpio_set_direction(c2_buttons[i], 1);
        gpio_set_die(c2_buttons[i], 1);
    }
    for (int i = 0; i < ARRAY_SIZE(c2_analog); i++) {
        gpio_set_pull_up(c2_analog[i], 0);
        gpio_set_pull_down(c2_analog[i], 0);
        gpio_set_direction(c2_analog[i], 1);
        gpio_set_die(c2_analog[i], c2_analog[i] == C2_AN_PA12);   /* stock: PA12 keeps digital in */
    }
    gpio_set_pull_up(C2_5V_DETECT, 0);
    gpio_set_pull_down(C2_5V_DETECT, 0);
    gpio_set_direction(C2_5V_DETECT, 1);
    gpio_set_die(C2_5V_DETECT, 1);
    gpio_set_pull_up(C2_STICK_RX, 1);           /* the stick chip's UART line: unused in phase 3 */
    gpio_set_direction(C2_STICK_RX, 1);
    gpio_set_die(C2_STICK_RX, 1);

    c2_out(C2_MOTOR_A, 0);                      /* stock: rumble PWM pins start low */
    c2_out(C2_MOTOR_B, 0);
    c2_out(C2_PA08_OUT, 0);                     /* stock init levels */
    c2_out(C2_PEDESTAL_PWR, 0);
    c2_out(C2_EXT_POWER, 1);                    /* stock: high at init ... */
}

extern void led_spi_init(void);
extern void nvram_set_boot_state(u32 state);
extern void hw_mmu_disable(void);
extern void ram_protect_close(void);

/* ---- Back to the USB bootloader, as stock (0x1E34AB4 / 0x6D70) and the SDK's
 * go_mask_usb_updata(): boot state UPGRADE_USB_SOFT_KEY (2), then a soft
 * reset; the ROM comes up as the JieLi USB bootloader (4c4a:2342). Runs from
 * RAM: the MMU (flash mapping) is off by then. */
AT(.volatile_ram_code)
static void c2_enter_isp(void)
{
    local_irq_disable();
    ram_protect_close();
    hw_mmu_disable();
    nvram_set_boot_state(2);
    JL_CLOCK->PWR_CON |= BIT(4);
    while (1);
}

/* The same from the debug log's "boot" command (core/gp_log.c). */
void gp_board_enter_bootloader(void)
{
    c2_enter_isp();
}

/* Stock's combo: Home + Options held with 5 V present. */
static int c2_isp_combo(void)
{
    return !gpio_read(C2_KEY_HOME) && !gpio_read(C2_KEY_OPTIONS) && gpio_read(C2_5V_DETECT);
}

/* LED chain: see gamepad_c2.c. The grips are dark for the first frame
 * unless "extend power" (PB09) has been on for a while: the whole chain is
 * powered from it (with PB09 off it stayed dark, 2026-11-13). */

/* Polled every 20 ms: Home + Options held for 2 s with 5 V present enters
 * the USB bootloader. (The step-2 LED test that used to live here is
 * summarised above and in docs/firmware-design.md.) */
static void c2_board_poll(void *priv)
{
    static u8 isp_ticks;
    isp_ticks = c2_isp_combo() ? isp_ticks + 1 : 0;
    if (isp_ticks >= 100) {
        c2_enter_isp();
    }
}

/* Busy-wait on TIMER0, as cpu/br23/charge.c's udelay() (delay_us() is
 * declared in the SDK headers but no library linked here defines it). */
static void c2_udelay(u32 usec)
{
    JL_TIMER0->CON = BIT(14);
    JL_TIMER0->CNT = 0;
    JL_TIMER0->PRD = clk_get("lsb") / 1000000L * usec;
    JL_TIMER0->CON = BIT(0);
    while ((JL_TIMER0->CON & BIT(15)) == 0);
    JL_TIMER0->CON = BIT(14);
}

static void c2_led_boot_indicator(void)
{
    c2_out(C2_EXT_POWER, 0);                    /* ... and "open extend power" drives it low (0x1E348FE) */
    for (int i = 0; i < 10; i++) {              /* 10 ms: the grip LEDs run from it, let them power up */
        c2_udelay(1000);
    }
    led_spi_init();                             /* PC05 as SPI1 DO, the SPI1 interrupt, CON */
    /* Home dim white: booted. Not when woken by 5 V alone (USB or the dock):
     * that boot goes straight to the charging animation, and the white
     * showed as a flash before it (2026-11-12). */
    static const u8 frame[WS2812_MAX][3] = { { 16, 16, 16 } }, dark[WS2812_MAX][3];
    ws2812_spi_send(gp_board_woken_by_usb_only() ? dark : frame, WS2812_MAX);
    usr_timer_add(NULL, c2_board_poll, 20, 1);
}

/* Why the chip started (logged at boot: 0x40 soft-off wake / flash, 0x04
 * watchdog). Read before anything else touches the PMU. */
u8 c2_boot_rst_src, c2_boot_wkup_pnd;

void board_init()
{
    c2_boot_rst_src = p33_rx_1byte(P3_RST_SRC);
    c2_boot_wkup_pnd = p33_rx_1byte(P3_WKUP_PND);
    c2_gpio_init();
    for (volatile int i = 0; i < 2000; i++);   /* let the pull-ups settle */
    if (c2_isp_combo()) {                       /* stock: "power on enter usb updata mode" */
        c2_enter_isp();
    }
    board_power_init();
    adc_vbg_init();
    adc_init();
    adc_add_sample_ch(AD_CH_PB1);               /* C2: the battery (1:2 divider), before the SDK's battery check reads it */
    cfg_file_parse(0);
    devices_init();

	board_devices_init();
	c2_led_boot_indicator();

#if TCFG_CHARGE_ENABLE
    charge_api_init((void *)&charge_data);
#if TCFG_HANDSHAKE_ENABLE
    if(get_charge_online_flag()){
        handshake_app_start(0, NULL);
    }
#endif

#else
	/*close FAST CHARGE */
	CHARGE_EN(0);
	CHGBG_EN(0);
#endif

	log_info("board_init");

	if(get_charge_online_flag()){
		log_info("charge...select LDO15");
		power_set_mode(PWR_LDO15);
	}else{
		power_set_mode(TCFG_LOWPOWER_POWER_SEL);
	}

}

/************************** DAC ****************************/
#if TCFG_AUDIO_DAC_ENABLE
struct dac_platform_data dac_data = {
    .ldo_volt       = TCFG_AUDIO_DAC_LDO_VOLT,                   //DACVDD等级.需要根据具体硬件来设置（高低压）可选:1.2V/1.3V/2.35V/2.5V/2.65V/2.8V/2.95V/3.1V
#if ((TCFG_AUDIO_DAC_CONNECT_MODE == DAC_OUTPUT_FRONT_LR_REAR_LR) || (TCFG_AUDIO_DAC_CONNECT_MODE == DAC_OUTPUT_DUAL_LR_DIFF))
    .vcmo_en        = 0,                                         //四声道与双声道差分关闭VCOMO
#else
    .vcmo_en        = 0,                                         //是否打开VCOMO
#endif
    .output         = TCFG_AUDIO_DAC_CONNECT_MODE,               //DAC输出配置，和具体硬件连接有关，需根据硬件来设置
    .ldo_isel       = 3,
    .ldo_fb_isel    = 2,
    .lpf_isel       = 0x8,
};
#endif

/************************** ADC ****************************/
#if TCFG_AUDIO_ADC_ENABLE
const struct ladc_port ladc_list[] = {
	{// 0
		.channel = TCFG_AUDIO_ADC_LINE_CHA0,
	},
	{// 1
		.channel = TCFG_AUDIO_ADC_LINE_CHA1,
	},
	// total must < 4
};
struct adc_platform_data adc_data = {
	.mic_channel    = TCFG_AUDIO_ADC_MIC_CHA,                   //MIC通道选择，对于693x，MIC只有一个通道，固定选择右声道
/*MIC LDO电流档位设置：
    0:0.625ua    1:1.25ua    2:1.875ua    3:2.5ua*/
	.mic_ldo_isel   = TCFG_AUDIO_ADC_LDO_SEL,
/*MIC 是否省隔直电容：
    0: 不省电容  1: 省电容 */
#if ((TCFG_AUDIO_DAC_CONNECT_MODE == DAC_OUTPUT_FRONT_LR_REAR_LR) || (TCFG_AUDIO_DAC_CONNECT_MODE == DAC_OUTPUT_DUAL_LR_DIFF))
	.mic_capless    = 0,//四声道与双声道差分使用，不省电容接法
#else
	.mic_capless    = 0,
#endif
/*MIC内部上拉电阻挡位配置，影响MIC的偏置电压
    21:1.18K	20:1.42K 	19:1.55K 	18:1.99K 	17:2.2K 	16:2.4K 	15:2.6K		14:2.91K	13:3.05K 	12:3.5K 	11:3.73K
	10:3.91K  	9:4.41K 	8:5.0K  	7:5.6K		6:6K		5:6.5K		4:7K		3:7.6K		2:8.0K		1:8.5K				*/
    .mic_bias_res   = 16,
/*MIC LDO电压档位设置,也会影响MIC的偏置电压
    0:2.3v  1:2.5v  2:2.7v  3:3.0v */
	.mic_ldo_vsel  = 2,
/*MIC电容隔直模式使用内部mic偏置(PC7)*/
	.mic_bias_inside = 1,
/*保持内部mic偏置输出*/
	.mic_bias_keep = 0,

	// ladc 通道
    .ladc_num = ARRAY_SIZE(ladc_list),
    .ladc = ladc_list,
};
#endif

/************************** PWR config ****************************/
/* C2 wake-up sources, as stock's wakeup_param 0x1E5B2E0 (c2_power.h). */
struct port_wakeup port0 = {
	.pullup_down_enable = ENABLE,
	.edge               = FALLING_EDGE,
	.attribute          = BLUETOOTH_RESUME,
	.iomap              = C2_WAKE_HOME,                      /* PB11 */
    .filter_enable      = DISABLE,
};

struct port_wakeup port_5v = {
	.pullup_down_enable = DISABLE,
	.edge               = RISING_EDGE,
	.attribute          = BLUETOOTH_RESUME,
	.iomap              = C2_WAKE_5V,                        /* PC03 */
    .filter_enable      = DISABLE,
};

const struct sub_wakeup sub_wkup = {
	.attribute  = BLUETOOTH_RESUME,
};

const struct charge_wakeup charge_wkup = {
	.attribute  = BLUETOOTH_RESUME,
};

const struct wakeup_param wk_param = {
	.port[1] = &port0,
	.port[2] = &port_5v,
	.sub = &sub_wkup,
	.charge = &charge_wkup,
};


int get_sfc_bit_mode(void);   /* the SDK's library (no header declares it): 2 or 4 data lines */

/*进软关机之前默认将IO口都设置成高阻状态，需要保留原来状态的请修改该函数*/
static void close_gpio(u8 is_softoff)
{
    u16 port_group[] = {
        [PORTA_GROUP] = 0xffff & ~(BIT(13)|BIT(14)),
        [PORTB_GROUP] = 0xffff,
        [PORTC_GROUP] = 0xffff,
    };

    /*
       u8 mode = (JL_SFC->CON >> 8) & 0x0f;
       extern u32 spi_get_port(void);
       if (spi_get_port() != 0) {
       if ((mode == 0b0101) || (mode == 0b0111)) {
       port_group[PORTA_GROUP] = 0x1fff;
       } else {
       port_group[PORTA_GROUP] = 0x9fff;
       }
       }
     */

#if TCFG_IOKEY_ENABLE
	port_protect(port_group, TCFG_IOKEY_POWER_ONE_PORT);
    /* port_protect(port_group, TCFG_IOKEY_PREV_ONE_PORT); */
    /* port_protect(port_group, TCFG_IOKEY_NEXT_ONE_PORT); */
#endif /* TCFG_IOKEY_ENABLE */

#if TCFG_ADKEY_ENABLE
    port_protect(port_group,TCFG_ADKEY_PORT);
#endif

#if TCFG_CHARGE_ENABLE && TCFG_HANDSHAKE_ENABLE
    if (is_softoff == 0) {
        port_protect(port_group, TCFG_HANDSHAKE_IO_DATA1);
        port_protect(port_group, TCFG_HANDSHAKE_IO_DATA2);
    }
#endif

    //< close gpio
    gpio_dir(GPIOA, 0, 16, port_group[PORTA_GROUP], GPIO_OR);
    gpio_set_pu(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);
    gpio_set_pd(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);
    gpio_die(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);
    gpio_dieh(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);

    gpio_dir(GPIOB, 0, 16, port_group[PORTB_GROUP], GPIO_OR);
    gpio_set_pu(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);
    gpio_set_pd(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);
    gpio_die(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);
    gpio_dieh(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);


    gpio_dir(GPIOC, 0, 16, port_group[PORTC_GROUP], GPIO_OR);
    gpio_set_pu(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);
    gpio_set_pd(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);
    gpio_die(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);
    gpio_dieh(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);

    //< close usb io
    usb_iomode(1);
}


//-----------------------------------------------

/*进软关机之前默认将IO口都设置成高阻状态，需要保留原来状态的请修改该函数*/
extern void dac_power_off(void);

void board_set_soft_poweroff(void)
{
    u16 port_group[] = {
        [PORTA_GROUP] = 0xffff,
        [PORTB_GROUP] = 0xffff,
        [PORTC_GROUP] = 0xffff,
		[PORTD_GROUP] = 0Xffff,
    };

	if(TCFG_LOWPOWER_POWER_SEL == PWR_DCDC15){
		power_set_mode(PWR_LDO15);
	}

	//flash电源
	if(spi_get_port()==0){
		port_protect(port_group, SPI0_PWR_A);
		port_protect(port_group, SPI0_CS_A);
		port_protect(port_group, SPI0_CLK_A);
		port_protect(port_group, SPI0_DO_D0_A);
		port_protect(port_group, SPI0_DI_D1_A);
		if(get_sfc_bit_mode()==4){
			port_protect(port_group, SPI0_WP_D2_A);
			port_protect(port_group, SPI0_HOLD_D3_A);
		}
	}else{
		port_protect(port_group, SPI0_PWR_B);
		port_protect(port_group, SPI0_CS_B);
		port_protect(port_group, SPI0_CLK_B);
		port_protect(port_group, SPI0_DO_D0_B);
		port_protect(port_group, SPI0_DI_D1_B);
		if(get_sfc_bit_mode()==4){
			port_protect(port_group, SPI0_WP_D2_B);
			port_protect(port_group, SPI0_HOLD_D3_B);
		}
	}

	/* C2: keep the wake-up pins as they are, and the motor outputs driven low */
	port_protect(port_group, C2_WAKE_HOME);
	port_protect(port_group, C2_WAKE_5V);
	port_protect(port_group, C2_MOTOR_A);
	port_protect(port_group, C2_MOTOR_B);

//adkey / io可以作为唤醒口保留
#if TCFG_IOKEY_ENABLE
	port_protect(port_group, TCFG_IOKEY_POWER_ONE_PORT);
#endif

#if TCFG_ADKEY_ENABLE
    port_protect(port_group,TCFG_ADKEY_PORT);
#endif

    //< close gpio
    gpio_dir(GPIOA, 0, 16, port_group[PORTA_GROUP], GPIO_OR);
    gpio_set_pu(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);
    gpio_set_pd(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);
    gpio_die(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);
    gpio_dieh(GPIOA, 0, 16, ~port_group[PORTA_GROUP], GPIO_AND);

    gpio_dir(GPIOB, 0, 16, port_group[PORTB_GROUP], GPIO_OR);
    gpio_set_pu(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);
    gpio_set_pd(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);
    gpio_die(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);
    gpio_dieh(GPIOB, 0, 16, ~port_group[PORTB_GROUP], GPIO_AND);

    gpio_dir(GPIOC, 0, 16, port_group[PORTC_GROUP], GPIO_OR);
    gpio_set_pu(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);
    gpio_set_pd(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);
    gpio_die(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);
    gpio_dieh(GPIOC, 0, 16, ~port_group[PORTC_GROUP], GPIO_AND);

	gpio_dir(GPIOD, 0, 16, port_group[PORTD_GROUP], GPIO_OR);
    gpio_set_pu(GPIOD, 0, 16, ~port_group[PORTD_GROUP], GPIO_AND);
    gpio_set_pd(GPIOD, 0, 16, ~port_group[PORTD_GROUP], GPIO_AND);
    gpio_die(GPIOD, 0, 16, ~port_group[PORTD_GROUP], GPIO_AND);
    gpio_dieh(GPIOD, 0, 16, ~port_group[PORTD_GROUP], GPIO_AND);

    usb_iomode(1);

    gpio_set_pull_up(IO_PORT_DP, 0);
    gpio_set_pull_down(IO_PORT_DP, 0);
    gpio_set_direction(IO_PORT_DP, 1);
    gpio_set_die(IO_PORT_DP, 0);
    gpio_set_dieh(IO_PORT_DP, 0);

    gpio_set_pull_up(IO_PORT_DM, 0);
    gpio_set_pull_down(IO_PORT_DM, 0);
    gpio_set_direction(IO_PORT_DM, 1);
    gpio_set_die(IO_PORT_DM, 0);
    gpio_set_dieh(IO_PORT_DM, 0);



	/* dac_power_off(); */
}

#define     APP_IO_DEBUG_0(i,x)       //{JL_PORT##i->DIR &= ~BIT(x), JL_PORT##i->OUT &= ~BIT(x);}
#define     APP_IO_DEBUG_1(i,x)       //{JL_PORT##i->DIR &= ~BIT(x), JL_PORT##i->OUT |= BIT(x);}


void sleep_exit_callback(u32 usec)
{
	putchar('>');
    APP_IO_DEBUG_1(A, 6);

	if(TCFG_LOWPOWER_POWER_SEL == PWR_DCDC15){
		/* putchar('}'); */
		power_set_mode(TCFG_LOWPOWER_POWER_SEL);
	}

}

void sleep_enter_callback(u8  step)
{
    /* 此函数禁止添加打印 */
    if (step == 1) {
		putchar('<');
        APP_IO_DEBUG_0(A, 6);
#if TCFG_AUDIO_ENABLE
        dac_power_off();
#endif/*TCFG_AUDIO_ENABLE*/
		if(TCFG_LOWPOWER_POWER_SEL == PWR_DCDC15){
			/* putchar('{'); */
			power_set_mode(PWR_LDO15);
		}
    } else {

		close_gpio(0);

        gpio_set_pull_up(IO_PORT_DP, 0);
        gpio_set_pull_down(IO_PORT_DP, 0);
        gpio_set_direction(IO_PORT_DP, 1);
        gpio_set_die(IO_PORT_DP, 0);

        gpio_set_pull_up(IO_PORT_DM, 0);
        gpio_set_pull_down(IO_PORT_DM, 0);
        gpio_set_direction(IO_PORT_DM, 1);
        gpio_set_die(IO_PORT_DM, 0);
	}
}



void board_power_init(void)
{
    log_info("Power init : %s", __FILE__);

    /* C2: stock picks "wire" / "wireless" power params by the 5 V detect */
    power_param.vddiom_lev = gpio_read(C2_5V_DETECT) ? VDDIOM_VOL_32V : VDDIOM_VOL_28V;
    power_init(&power_param);

    gpio_longpress_pin0_reset_config(IO_PORTB_01, 0, 0);
    gpio_shortpress_reset_config(0);//1--enable 0--disable

    /*sdpg_config(1);*/

    power_set_callback(TCFG_LOWPOWER_LOWPOWER_SEL, sleep_enter_callback, sleep_exit_callback, board_set_soft_poweroff);

	power_keep_dacvdd_en(0);

	power_wakeup_init(&wk_param);




#if (!TCFG_IOKEY_ENABLE && !TCFG_ADKEY_ENABLE)
    charge_check_and_set_pinr(0);
#endif
}
#endif
