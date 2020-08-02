#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

#include "flicker_free.h"

static struct proc_dir_entry *root_entry, *enabled, *minbright;

static int show_ff_enabled(struct seq_file *seq, void *v)
{
	seq_printf(seq, "%d\n", if_flicker_free_enabled() ? 1 : 0);
	return 0;
}

static int my_open_ff_enabled(struct inode *inode, struct file *file)
{
	return single_open(file, show_ff_enabled, NULL);
}

static ssize_t my_write_procmem(struct file *file, const char __user *buffer,
                            size_t count, loff_t *pos)
{
	int value = 0;
	get_user(value, buffer);
	set_flicker_free(value != '0');
	return count;
}

static ssize_t my_write_procbright(struct file *file, const char __user *buffer,
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

	set_elvss_off_threshold(value);
end:
	kfree(tmp);
	return ret ? EFAULT : count;
}

static int show_procbright(struct seq_file *seq, void *v)
{
	seq_printf(seq, "%d\n", get_elvss_off_threshold());
	return 0;
}

static int my_open_procbright(struct inode *inode, struct file *file)
{
	return single_open(file, show_procbright, NULL);
}

static const struct file_operations proc_file_fops_enable = {
	.owner = THIS_MODULE,
	.open = my_open_ff_enabled,
	.read = seq_read,
	.write = my_write_procmem,
	.llseek = seq_lseek,
	.release = single_release,
};

static const struct file_operations proc_file_fops_minbright = {
	.owner = THIS_MODULE,
	.open = my_open_procbright,
	.read = seq_read,
	.write = my_write_procbright,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init ff_enable_init(void)
{
	root_entry = proc_mkdir("flicker_free", NULL);

	enabled = proc_create("flicker_free", 0x0666, root_entry,
		&proc_file_fops_enable);
	if (!enabled)
		return -EINVAL;

	minbright = proc_create("min_brightness", 0x0666, root_entry,
		&proc_file_fops_minbright);
	if (!minbright)
		return -EINVAL;

	return 0;
}

static void __exit ff_enable_exit(void)
{
	if (enabled)
		remove_proc_entry("flicker_free", root_entry);
	if (minbright)
		remove_proc_entry("min_brightness", root_entry);
}

module_init(ff_enable_init);
module_exit(ff_enable_exit);
