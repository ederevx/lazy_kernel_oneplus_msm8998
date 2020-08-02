/*
 * A flicker free driver based on Qcom MDSS for OLED devices
 *
 * Copyright (C) 2012-2014, The Linux Foundation. All rights reserved.
 * Copyright (C) Sony Mobile Communications Inc. All rights reserved.
 * Copyright (C) 2014-2018, AngeloGioacchino Del Regno <kholk11@gmail.com>
 * Copyright (C) 2018, Devries <therkduan@gmail.com>
 * Copyright (C) 2019-2020, Tanish <tanish2k09.dev@gmail.com>
 * Copyright (C) 2020, shxyke <shxyke@gmail.com>
 * Copyright (C) 2020, ederekun <sedrickvince@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/rtc.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/delay.h>

#include "flicker_free.h"
#include "mdss_fb.h"

#include "mdss_mdp.h"

/* Maximum value of RGB possible */
#define FF_MAX_SCALE 32768 
/* Minimum value of RGB recommended */
#define FF_MIN_SCALE 2560 

#define RET_WORKGROUND 1
#define RET_WORKGROUND_DELAY 200

#define BACKLIGHT_INDEX 66

static const int bkl_to_pcc[BACKLIGHT_INDEX] =
	{42, 56, 67, 75, 84, 91, 98, 104, 109, 114, 119, 124, 128, 133, 136,
	140, 143, 146, 150, 152, 156, 159, 162, 165, 168, 172, 176, 178, 181,
	184, 187, 189, 192, 194, 196, 199, 202, 204, 206, 209, 211, 213, 215,
	217, 220, 222, 224, 226, 228, 230, 233, 236, 237, 239, 241, 241, 243,
	245, 246, 249, 249, 250, 252, 254, 255, 256};

/* Minimum backlight value that does not flicker */
static int elvss_off_threshold = 66;

struct mdss_panel_data *pdata;
struct mdp_pcc_cfg_data pcc_config;
struct mdp_pcc_data_v1_7 *payload;
struct mdp_dither_cfg_data dither_config;
struct mdp_dither_data_v1_7 *dither_payload;
static u32 backlight = 0;
static const u32 pcc_depth[9] = {128, 256, 512, 1024, 2048,
	4096, 8192, 16384, 32768};
static u32 depth = 8;
static bool pcc_enabled = false;
static bool mdss_backlight_enable = false;
u32 copyback = 0;
u32 dither_copyback = 0;

#ifdef RET_WORKGROUND
#define BACK_TO_BL_DELAY msecs_to_jiffies(RET_WORKGROUND_DELAY - 62)
#define BACK_TO_PCC_DELAY msecs_to_jiffies(RET_WORKGROUND_DELAY + 80)

static struct delayed_work back_to_backlight_work, back_to_pcc_work;
#endif

static int flicker_free_push_dither(int depth)
{
	dither_config.flags = mdss_backlight_enable ?
		MDP_PP_OPS_WRITE | MDP_PP_OPS_ENABLE :
			MDP_PP_OPS_WRITE | MDP_PP_OPS_DISABLE;

	dither_config.r_cr_depth = depth;
	dither_config.g_y_depth = depth;
	dither_config.b_cb_depth = depth;

	dither_payload->len = 0;
	dither_payload->temporal_en = 0;

	dither_payload->r_cr_depth = dither_config.r_cr_depth;
	dither_payload->g_y_depth = dither_config.g_y_depth;
	dither_payload->b_cb_depth = dither_config.b_cb_depth;

	dither_config.cfg_payload = dither_payload;
	return mdss_mdp_dither_config(get_mfd_copy(), &dither_config, &dither_copyback, 1);
}

static int flicker_free_push_pcc(int temp)
{
	pcc_config.ops = pcc_enabled ?
		MDP_PP_OPS_WRITE | MDP_PP_OPS_ENABLE :
			MDP_PP_OPS_WRITE | MDP_PP_OPS_DISABLE;

	pcc_config.r.r = temp;
	pcc_config.g.g = temp;
	pcc_config.b.b = temp;

	payload->r.r = pcc_config.r.r;
	payload->g.g = pcc_config.g.g;
	payload->b.b = pcc_config.b.b;

	pcc_config.cfg_payload = payload;
	return mdss_mdp_kernel_pcc_config(get_mfd_copy(), &pcc_config, &copyback);
}

static int set_brightness(int backlight)
{
	u32 temp = 0;

	backlight = clamp_t(int, (((backlight - 1) * (BACKLIGHT_INDEX - 1)) /
		(elvss_off_threshold - 1)) + 1, 1, BACKLIGHT_INDEX);
	temp = clamp_t(int, 0x80 * bkl_to_pcc[backlight - 1], FF_MIN_SCALE,
		FF_MAX_SCALE);

	for (depth = 8; depth >= 1; depth--) {
		if (temp >= pcc_depth[depth])
			break;
	}

	flicker_free_push_dither(depth);
	return flicker_free_push_pcc(temp);
}

u32 mdss_panel_calc_backlight(u32 bl_lvl)
{
	if (bl_lvl != 0) {
		if (mdss_backlight_enable && bl_lvl < elvss_off_threshold) {
			printk("flicker free mode on\n");
			printk("elvss_off = %d\n", elvss_off_threshold);
			pcc_enabled = true;
			if (!set_brightness(bl_lvl))
				return elvss_off_threshold;
		} else if (pcc_enabled) {
			pcc_enabled = false;
			set_brightness(elvss_off_threshold);
		}
	}

	return bl_lvl;
}

#ifdef RET_WORKGROUND
static void back_to_backlight(struct work_struct *work)
{
	pdata = dev_get_platdata(&get_mfd_copy()->pdev->dev);
	pdata->set_backlight(pdata, backlight);
}

static void back_to_pcc(struct work_struct *work)
{
	mdss_panel_calc_backlight(backlight);
}
#endif

void set_flicker_free(bool enabled)
{
	if (mdss_backlight_enable == enabled)
		return;

	mdss_backlight_enable = enabled;

	if (!get_mfd_copy())
		return;

	pdata = dev_get_platdata(&get_mfd_copy()->pdev->dev);
	if (!pdata || !pdata->set_backlight)
		return;

	backlight = enabled ? mdss_panel_calc_backlight(get_bkl_lvl()) :
		get_bkl_lvl();

#ifdef RET_WORKGROUND
	if (enabled) {
		mod_delayed_work(system_wq, &back_to_backlight_work, BACK_TO_BL_DELAY);
	} else {
		pdata->set_backlight(pdata, backlight);
		mod_delayed_work(system_wq, &back_to_pcc_work, BACK_TO_PCC_DELAY);
	}
#else
	pdata->set_backlight(pdata, backlight);
	if (!enabled)
		mdss_panel_calc_backlight(backlight);
#endif
}

void set_elvss_off_threshold(int value)
{
	elvss_off_threshold = value;
}

int get_elvss_off_threshold(void)
{
	return elvss_off_threshold;
}

bool if_flicker_free_enabled(void)
{
	return mdss_backlight_enable;
}

static int __init flicker_free_init(void)
{
	memset(&pcc_config, 0, sizeof(struct mdp_pcc_cfg_data));

	pcc_config.version = mdp_pcc_v1_7;
	pcc_config.block = MDP_LOGICAL_BLOCK_DISP_0;
	payload = kzalloc(sizeof(struct mdp_pcc_data_v1_7),GFP_USER);

	memset(&dither_config, 0, sizeof(struct mdp_dither_cfg_data));

	dither_config.version = mdp_dither_v1_7;
	dither_config.block = MDP_LOGICAL_BLOCK_DISP_0;
	dither_payload = kzalloc(sizeof(struct mdp_dither_data_v1_7), GFP_USER);

#ifdef RET_WORKGROUND
	INIT_DELAYED_WORK(&back_to_backlight_work, back_to_backlight);
	INIT_DELAYED_WORK(&back_to_pcc_work, back_to_pcc);
#endif

	return 0;
}

static void __exit flicker_free_exit(void)
{
	kfree(payload);
	kfree(dither_payload);
}

late_initcall(flicker_free_init);
module_exit(flicker_free_exit);
