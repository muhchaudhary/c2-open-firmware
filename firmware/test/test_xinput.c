/* test_xinput.c: the wired Xbox 360 report (core/gp_xinput_desc.c). */
#include "test.h"
#include "gp_xinput.h"

static s16 get_s16(const u8 *p)
{
    return (s16)(p[0] | p[1] << 8);
}

static u16 buttons_of(u32 gp_buttons)
{
    struct gp_state s = { .buttons = gp_buttons };
    u8 rep[XINPUT_REPORT_LEN];
    xinput_encode_state(rep, &s);
    return rep[2] | rep[3] << 8;
}

static void header(void)
{
    struct gp_state s = { 0 };
    u8 rep[XINPUT_REPORT_LEN];
    xinput_encode_state(rep, &s);
    CHECK_EQ(rep[0], 0x00);                              /* message type: input */
    CHECK_EQ(rep[1], XINPUT_REPORT_LEN);
    CHECK_EQ(XINPUT_REPORT_LEN, 20);
}

static void button_bits(void)
{
    /* The XInput wButtons layout. */
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_UP)), 0x0001);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_DOWN)), 0x0002);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_LEFT)), 0x0004);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_RIGHT)), 0x0008);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_START)), 0x0010);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_BACK)), 0x0020);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_L3)), 0x0040);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_R3)), 0x0080);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_L1)), 0x0100);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_R1)), 0x0200);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_GUIDE)), 0x0400);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_SOUTH)), 0x1000);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_EAST)), 0x2000);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_WEST)), 0x4000);
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_NORTH)), 0x8000);
    /* No XInput equivalent: dropped, not mapped onto something else. */
    CHECK_EQ(buttons_of(GP_BIT(GP_BTN_PADDLE_L1) | GP_BIT(GP_BTN_PADDLE_R1) | GP_BIT(GP_BTN_CAPTURE) |
                        GP_BIT(GP_BTN_L2) | GP_BIT(GP_BTN_R2) | GP_BIT(GP_BTN_MODIFIER)), 0);
}

static void triggers(void)
{
    struct gp_state s = { .lt = -32768, .rt = 32767 };
    u8 rep[XINPUT_REPORT_LEN];
    xinput_encode_state(rep, &s);
    CHECK_EQ(rep[4], 0);
    CHECK_EQ(rep[5], 255);
    s.lt = 0;
    xinput_encode_state(rep, &s);
    CHECK_EQ(rep[4], 128);
}

static void sticks_y_flipped(void)
{
    /* SInput's +Y is down, XInput's up: Y flips, X doesn't. */
    struct gp_state s = { .lx = 32767, .ly = 32767, .rx = -32767, .ry = -32767 };
    u8 rep[XINPUT_REPORT_LEN];
    xinput_encode_state(rep, &s);
    CHECK_EQ(get_s16(rep + 6), 32767);
    CHECK_EQ(get_s16(rep + 8), -32767);
    CHECK_EQ(get_s16(rep + 10), -32767);
    CHECK_EQ(get_s16(rep + 12), 32767);
    s.ly = -32768;                                       /* can't be negated in 16 bits: clamps */
    xinput_encode_state(rep, &s);
    CHECK_EQ(get_s16(rep + 8), 32767);
}

int main(void)
{
    printf("test_xinput\n");
    RUN(header);
    RUN(button_bits);
    RUN(triggers);
    RUN(sticks_y_flipped);
    return t_done("test_xinput");
}
