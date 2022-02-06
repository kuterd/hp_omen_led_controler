/*
 * HP WMI RGB Keyboard controller
 * Copyright (C) 2022 Kuter Dinel <kuterdinel@gmail.com>
 *
 * Portions based on hp-wmi.c:
 * Copyright (C) 2008 Red Hat <mjg@redhat.com>
 * Copyright (C) 2010, 2011 Anssi Hannula <anssi.hannula@iki.fi>
 *
 * Portions based on wistron_btns.c:
 * Copyright (C) 2005 Miloslav Trmac <mitr@volny.cz>
 * Copyright (C) 2005 Bernhard Rosenkraenzer <bero@arklinux.org>
 * Copyright (C) 2005 Dmitry Torokhov <dtor@mail.ru>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/input.h>
#include <linux/hwmon.h>
#include <linux/acpi.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/led-class-multicolor.h>

MODULE_AUTHOR("Kuter Dinel <kuterdinel@gmail.com>");
MODULE_DESCRIPTION("HP RGB Keyboard driver");
MODULE_LICENSE("GPL");

#define HPWMI_BIOS_GUID "5FB7F034-2C63-45e9-BE91-3D44E2C707E4"

struct bios_args {
	u32 signature;
	u32 command;
	u32 commandtype;
	u32 datasize;
	u8 data[128];
};

struct bios_return {
	u32 sigpass;
	u32 return_code;
};

enum hp_return_value {
	HPWMI_RET_WRONG_SIGNATURE	= 0x02,
	HPWMI_RET_UNKNOWN_COMMAND	= 0x03,
	HPWMI_RET_UNKNOWN_CMDTYPE	= 0x04,
	HPWMI_RET_INVALID_PARAMETERS	= 0x05,
};

enum hp_wmi_command {
	HPWMI_READ	= 0x01,
	HPWMI_WRITE	= 0x02,
	HPWMI_ODM	= 0x03,
	HPWMI_GM	= 0x20008,
    HPWMI_LIGHTING = 0x20009
};

enum hp_wmi_ligth_commandtype {
    HP_WMI_LIGHTING_GET_PLATFORM_INFO = 0x1,
    HP_WMI_LIGHTING_GET_ZONE_COLORS = 0x2,
    HP_WMI_LIGHTING_SET_ZONE_COLORS = 0x3,
    HP_WMI_LIGHTING_GET_STATUS = 0x4,
//    HP_WMI_LIGHTING_GET_KEYBOARD_TYPE = 0x2b,
    HP_WMI_LIGHTING_SET_BRIGHTNESS = 0x5
};

#define LED_COUNT 4
#define HP_WMI_KEYBOARD_COLOR_DATA_SIZE (LED_COUNT * 3)
#define HP_WMI_KEYBOARD_COLOR_MESSAGE_SIZE (25 + HP_WMI_KEYBOARD_COLOR_DATA_SIZE)

u8 keyboard_led_support = 0;
struct led_classdev_mc keyboard_leds[LED_COUNT] = {};
struct mc_subled keyboard_subleds[LED_COUNT * 3] = {};

/* map output size to the corresponding WMI method id */
static inline int encode_outsize_for_pvsz(int outsize)
{
	if (outsize > 4096)
		return -EINVAL;
	if (outsize > 1024)
		return 5;
	if (outsize > 128)
		return 4;
	if (outsize > 4)
		return 3;
	if (outsize > 0)
		return 2;
	return 1;
}

/*
 * taken from hp-wmi.c
 * hp_wmi_perform_query
 *
 * query:	The commandtype (enum hp_wmi_commandtype)
 * write:	The command (enum hp_wmi_command)
 * buffer:	Buffer used as input and/or output
 * insize:	Size of input buffer
 * outsize:	Size of output buffer
 *
 * returns zero on success
 *         an HP WMI query specific error code (which is positive)
 *         -EINVAL if the query was not successful at all
 *         -EINVAL if the output buffer size exceeds buffersize
 *
 * Note: The buffersize must at least be the maximum of the input and output
 *       size. E.g. Battery info query is defined to have 1 byte input
 *       and 128 byte output. The caller would do:
 *       buffer = kzalloc(128, GFP_KERNEL);
 *       ret = hp_wmi_perform_query(HPWMI_BATTERY_QUERY, HPWMI_READ, buffer, 1, 128)
 */
static int hp_wmi_perform_query(int query, enum hp_wmi_command command,
				void *buffer, int insize, int outsize)
{
	int mid;
	struct bios_return *bios_return;
	int actual_outsize;
	union acpi_object *obj;
	struct bios_args args = {
		.signature = 0x55434553,
		.command = command,
		.commandtype = query,
		.datasize = insize,
		.data = { 0 },
	};
	struct acpi_buffer input = { sizeof(struct bios_args), &args };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	int ret = 0;

	mid = encode_outsize_for_pvsz(outsize);
	if (WARN_ON(mid < 0))
		return mid;

	if (WARN_ON(insize > sizeof(args.data)))
		return -EINVAL;
	memcpy(&args.data[0], buffer, insize);

	wmi_evaluate_method(HPWMI_BIOS_GUID, 0, mid, &input, &output);

	obj = output.pointer;

	if (!obj)
		return -EINVAL;

	if (obj->type != ACPI_TYPE_BUFFER) {
		ret = -EINVAL;
		goto out_free;
	}

	bios_return = (struct bios_return *)obj->buffer.pointer;
	ret = bios_return->return_code;

	if (ret) {
		if (ret != HPWMI_RET_UNKNOWN_COMMAND &&
		    ret != HPWMI_RET_UNKNOWN_CMDTYPE)
			pr_warn("query 0x%x returned error 0x%x\n", query, ret);
		goto out_free;
	}

	/* Ignore output data of zero size */
	if (!outsize)
		goto out_free;

	actual_outsize = min(outsize, (int)(obj->buffer.length - sizeof(*bios_return)));
	memcpy(buffer, obj->buffer.pointer + sizeof(*bios_return), actual_outsize);
	memset(buffer + actual_outsize, 0, outsize - actual_outsize);

out_free:
	kfree(obj);
	return ret;
}

static int hp_wmi_check_rgb_keyboard_support(u8 *lightSupport) {
    int rc = hp_wmi_perform_query(HP_WMI_LIGHTING_GET_PLATFORM_INFO, HPWMI_LIGHTING, lightSupport, 1, 1);
    *lightSupport &= 1;

    if (rc)
        *lightSupport = 0;

    return rc;
}

static int hp_wmi_get_keyboard_type(u8 *keyboardType) {
    return hp_wmi_perform_query(43, HPWMI_GM, keyboardType, 1, 1);
}

static int hp_wmi_get_keyboard_status(u8 *keyboardStatus) {
    return hp_wmi_perform_query(HP_WMI_LIGHTING_GET_STATUS, HPWMI_LIGHTING, keyboardStatus, 1, 1);
}

static int hp_wmi_set_keyboard_brightness(u8 value) {
    u8 data[4] = {0};

    data[0] = value;
    return hp_wmi_perform_query(HP_WMI_LIGHTING_SET_BRIGHTNESS, HPWMI_LIGHTING, data, 4, 1);
}

// Out must be HP_WMI_KEYBOARD_COLOR_MESSAGE_SIZE bytes long
static int hp_wmi_keyboard_get_colors(u8 *out) {
    return hp_wmi_perform_query(HP_WMI_LIGHTING_GET_ZONE_COLORS, HPWMI_LIGHTING, out, 1, HP_WMI_KEYBOARD_COLOR_MESSAGE_SIZE);
}

// In must be HPWMI_KEYBOARD_COLOR_MESSAGE_SIZE bytes long
static int hp_wmi_keyboard_set_colors(u8 *in) {
    return hp_wmi_perform_query(HP_WMI_LIGHTING_SET_ZONE_COLORS, HPWMI_LIGHTING, in, HP_WMI_KEYBOARD_COLOR_MESSAGE_SIZE, 1);
}

int keyboard_set_brightness(struct led_classdev *led_cdev, enum led_brightness brightness) {
    u8 color_buffer[HP_WMI_KEYBOARD_COLOR_MESSAGE_SIZE] = {0};
    int i, rc;
	struct led_classdev_mc *mc_cdev = lcdev_to_mccdev(led_cdev);

	led_mc_calc_color_components(mc_cdev, brightness);

    rc = hp_wmi_keyboard_get_colors(color_buffer);
    if (rc)
      return rc;

    // Sleep for at least 5 milliseconds
    usleep_range(5000, 6000);

    //update keyboard leds.
    for (i = 0; i < LED_COUNT; i++) {
        color_buffer[25 + i * 3 + 0] = (u8)keyboard_leds[i].subled_info[0].brightness;
        color_buffer[25 + i * 3 + 1] = (u8)keyboard_leds[i].subled_info[1].brightness;
        color_buffer[25 + i * 3 + 2] = (u8)keyboard_leds[i].subled_info[2].brightness;
    }

    rc = hp_wmi_keyboard_set_colors(color_buffer);

    return rc;
}

int setup_leds(void) {
    u8 led_data[HP_WMI_KEYBOARD_COLOR_MESSAGE_SIZE];
    int rc, i;

    // Led color data is saved in hardware and will be remembered between restarts.
    // Fetch the current status of leds.
    rc = hp_wmi_keyboard_get_colors(led_data);
    if (rc)
        return rc;

    // Setup keyboard leds.
    for (i = 0; i < LED_COUNT; i++) {
        struct led_classdev_mc *led = &keyboard_leds[i];

        led->subled_info = &keyboard_subleds[i * 3];
        led->num_colors = 3;

        led->subled_info[0].color_index = LED_COLOR_ID_RED;
	    led->subled_info[1].color_index = LED_COLOR_ID_GREEN;
		led->subled_info[2].color_index = LED_COLOR_ID_BLUE;

        led->subled_info[0].brightness = led_data[25 + i * 3 + 0];
        led->subled_info[1].brightness = led_data[25 + i * 3 + 1];
        led->subled_info[2].brightness = led_data[25 + i * 3 + 2];

        led->led_cdev.name = kasprintf(GFP_KERNEL, "keyboard:rgb:zone%d", i);
        led->led_cdev.brightness = 255;
        led->led_cdev.max_brightness = 255;
        led->led_cdev.brightness_set_blocking = keyboard_set_brightness;

        rc = led_classdev_multicolor_register(NULL, led);
        if (rc)
            return rc;
    }

    return 0;
}

static int __init hp_wmi_init(void) {
    int rc;
    pr_info("HP Omen RGB Keyboard driver loaded");

    rc = wmi_has_guid(HPWMI_BIOS_GUID);

    if (!rc)
        return -ENODEV;

    pr_info("HP WMI Verified bios capability");

    rc = hp_wmi_check_rgb_keyboard_support(&keyboard_led_support);

    pr_info("Lighting support: %d", keyboard_led_support);

    if (keyboard_led_support)
        setup_leds();

    return rc;
}

static void __exit hp_wmi_exit(void) {
    int i;
    pr_info("HP Omen RGB Keyboard driver removed");

    if (!keyboard_led_support)
        return;

    for (i = 0; i < 4; i++) {
        struct led_classdev_mc *led = &keyboard_leds[i];
        led_classdev_multicolor_unregister(led);
        kfree_const(led->led_cdev.name);
    }
}

module_init(hp_wmi_init);
module_exit(hp_wmi_exit);
