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

#include <linux/fb.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/rtc.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

#include "mdss_fb.h"
#include "mdss_mdp.h"

#include "flicker_free.h"

/* Maximum value of RGB possible */
#define FF_MAX_SCALE 32768

/* Minimum value of RGB recommended */
#define FF_MIN_SCALE 2560 

/* Number of backlight entries */
#define BACKLIGHT_INDEX 66

/* Minimum backlight value that does not flicker */
static int elvss_off_threshold = 66;

/* Proc directory entries */
static struct proc_dir_entry *root_entry, *enabled, *minbright;

/* Display configuration data */
static const int bkl_to_pcc[BACKLIGHT_INDEX] =
	{42, 56, 67, 75, 84, 91, 98, 104, 109, 114, 119, 124, 128, 133, 136,
	140, 143, 146, 150, 152, 156, 159, 162, 165, 168, 172, 176, 178, 181,
	184, 187, 189, 192, 194, 196, 199, 202, 204, 206, 209, 211, 213, 215,
	217, 220, 222, 224, 226, 228, 230, 233, 236, 237, 239, 241, 241, 243,
	245, 246, 249, 249, 250, 252, 254, 255, 256};

static const uint32_t pcc_depth[9] = {128, 256, 512, 1024, 2048,
	4096, 8192, 16384, 32768};

static struct mdp_pcc_cfg_data pcc_config;
static struct mdp_dither_cfg_data dither_config;
static struct mdp_dither_data_v1_7 *dither_payload;
static struct mdp_pcc_data_v1_7 *payload;
struct msm_fb_data_type *ff_mfd_copy;

static uint32_t dither_copyback = 0;
static uint32_t copyback = 0;
uint32_t ff_bl_lvl_cpy;

/* State booleans */
static bool pcc_enabled = false;
static bool mdss_backlight_enable = false;

static int flicker_free_push(int val)
{
	uint32_t backlight, temp, depth;

	backlight = clamp_t(int, (((val - 1) * (BACKLIGHT_INDEX - 1)) /
					(elvss_off_threshold - 1)) + 1, 1, BACKLIGHT_INDEX);
	temp = clamp_t(int, 0x80 * bkl_to_pcc[backlight - 1], FF_MIN_SCALE,
					FF_MAX_SCALE);

	for (depth = 8; depth >= 1; depth--) {
		if (temp >= pcc_depth[depth])
			break;
	}

	/* Configure dither values */
	dither_config.flags = MDP_PP_OPS_WRITE | (mdss_backlight_enable ?
										MDP_PP_OPS_ENABLE : MDP_PP_OPS_DISABLE);

	dither_config.r_cr_depth = depth;
	dither_config.g_y_depth = depth;
	dither_config.b_cb_depth = depth;

	dither_payload->r_cr_depth = depth;
	dither_payload->g_y_depth = depth;
	dither_payload->b_cb_depth = depth;

	dither_payload->len = 0;
	dither_payload->temporal_en = 0;

	dither_config.cfg_payload = dither_payload;

	/* Configure pcc values */
	pcc_config.ops = MDP_PP_OPS_WRITE | (pcc_enabled ?
							MDP_PP_OPS_ENABLE : MDP_PP_OPS_DISABLE);

	pcc_config.r.r = temp;
	pcc_config.g.g = temp;
	pcc_config.b.b = temp;

	payload->r.r = temp;
	payload->g.g = temp;
	payload->b.b = temp;

	pcc_config.cfg_payload = payload;

	/* Push values consecutively */
	return mdss_mdp_dither_config(ff_mfd_copy, &dither_config, &dither_copyback, 1) ||
				mdss_mdp_kernel_pcc_config(ff_mfd_copy, &pcc_config, &copyback);
}

uint32_t mdss_panel_calc_backlight(uint32_t bl_lvl)
{
	if (mdss_backlight_enable && bl_lvl < elvss_off_threshold) {
		pcc_enabled = true;
		if (!flicker_free_push(bl_lvl))
			return elvss_off_threshold;
	} else if (pcc_enabled) {
		pcc_enabled = false;
		flicker_free_push(elvss_off_threshold);
	}

	return bl_lvl;
}

/*
 * Proc directory
 */
static ssize_t ff_write_proc(struct file *file, const char __user *buffer,
								size_t count, loff_t *pos)
{
	struct mdss_panel_data *pdata;
	int value = 0;
	bool state;

	get_user(value, buffer);
	state = value != '0';

	if (mdss_backlight_enable != state) {
		mdss_backlight_enable = state;

		if (!unlikely(ff_mfd_copy))
			goto end;

		pdata = dev_get_platdata(&ff_mfd_copy->pdev->dev);
		if (unlikely(!pdata || !pdata->set_backlight))
			goto end;

		pdata->set_backlight(pdata, ff_bl_lvl_cpy);
	}
end:
	return count;
}

static int show_ff_state(struct seq_file *seq, void *v)
{
	seq_printf(seq, "%d\n", mdss_backlight_enable);
	return 0;
}

static int open_ff_proc(struct inode *inode, struct file *file)
{
	return single_open(file, show_ff_state, NULL);
}

static const struct file_operations proc_file_fops_state = {
	.owner = THIS_MODULE,
	.open = open_ff_proc,
	.read = seq_read,
	.write = ff_write_proc,
	.llseek = seq_lseek,
	.release = single_release,
};

static ssize_t eot_write_proc(struct file *file, const char __user *buffer,
								size_t count, loff_t *pos)
{
	int ret, value = 0;
	char *tmp = kzalloc((count + 1), GFP_KERNEL);

	if (!tmp)
		return -ENOMEM;

	ret = copy_from_user(tmp, buffer, count);
	if (ret)
		goto end;

	ret = kstrtoint(tmp, 10, &value);
	if (ret)
		goto end;

	elvss_off_threshold = value;
end:
	kfree(tmp);
	return ret ? EFAULT : count;
}

static int show_eot_val(struct seq_file *seq, void *v)
{
	seq_printf(seq, "%d\n", elvss_off_threshold);
	return 0;
}

static int open_eot_proc(struct inode *inode, struct file *file)
{
	return single_open(file, show_eot_val, NULL);
}

static const struct file_operations proc_file_fops_eot = {
	.owner = THIS_MODULE,
	.open = open_eot_proc,
	.read = seq_read,
	.write = eot_write_proc,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init flicker_free_init(void)
{
	int ret = 0;

	/* Display config init */
	memset(&pcc_config, 0, sizeof(struct mdp_pcc_cfg_data));

	pcc_config.version = mdp_pcc_v1_7;
	pcc_config.block = MDP_LOGICAL_BLOCK_DISP_0;
	payload = kzalloc(sizeof(struct mdp_pcc_data_v1_7),GFP_USER);

	memset(&dither_config, 0, sizeof(struct mdp_dither_cfg_data));

	dither_config.version = mdp_dither_v1_7;
	dither_config.block = MDP_LOGICAL_BLOCK_DISP_0;
	dither_payload = kzalloc(sizeof(struct mdp_dither_data_v1_7), GFP_USER);

	/* File operations init */
	root_entry = proc_mkdir("flicker_free", NULL);

	enabled = proc_create("flicker_free", 0x0666, root_entry,
					&proc_file_fops_state);
	if (!enabled) {
		ret = -EINVAL;
		goto end;
	}

	minbright = proc_create("min_brightness", 0x0666, root_entry,
					&proc_file_fops_eot);
	if (!minbright)
		ret = -EINVAL;

end:
	return ret;
}

static void __exit flicker_free_exit(void)
{
	kfree(payload);
	kfree(dither_payload);
	remove_proc_entry("flicker_free", root_entry);
	remove_proc_entry("min_brightness", root_entry);
}

late_initcall(flicker_free_init);
module_exit(flicker_free_exit);
