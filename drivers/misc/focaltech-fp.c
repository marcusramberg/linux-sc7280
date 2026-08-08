// SPDX-License-Identifier: GPL-2.0-only
/*
 * FocalTech FT9362 fingerprint sensor driver — GPIO/IRQ management only.
 * TA interaction (capture, match) is handled from userspace via /dev/tee0.
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/compat.h>
#include <linux/irq.h>

#define FF_IOC_ENABLE_IRQ   _IO('f', 0x05)
#define FF_IOC_DISABLE_IRQ  _IO('f', 0x06)
#define FF_IOC_RESET_DEVICE _IO('f', 0x02)

struct focalfp_dev {
	struct miscdevice miscdev;
	struct pinctrl *pinctrl;
	struct pinctrl_state *st_rst_low;
	struct pinctrl_state *st_rst_high;
	struct pinctrl_state *st_irq;
	struct pinctrl_state *st_pwr_low;
	struct pinctrl_state *st_pwr_high;
	int irq;
	bool irq_enabled;
	atomic_t irq_fired;
	wait_queue_head_t irq_wq;
	struct wakeup_source *ws;
	spinlock_t irq_lock;
};

static irqreturn_t focalfp_irq_handler(int irq, void *data)
{
	struct focalfp_dev *fp = data;

	spin_lock(&fp->irq_lock);
	if (fp->irq_enabled) {
		disable_irq_nosync(fp->irq);
		fp->irq_enabled = false;
		__pm_stay_awake(fp->ws);
		atomic_set(&fp->irq_fired, 1);
		wake_up_interruptible(&fp->irq_wq);
	}
	spin_unlock(&fp->irq_lock);
	return IRQ_HANDLED;
}

static int focalfp_do_reset(struct focalfp_dev *fp)
{
	int ret;

	ret = pinctrl_select_state(fp->pinctrl, fp->st_rst_low);
	if (ret)
		return ret;
	msleep(10);
	ret = pinctrl_select_state(fp->pinctrl, fp->st_rst_high);
	if (ret)
		return ret;
	msleep(50);
	return 0;
}

static long focalfp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct focalfp_dev *fp = container_of(file->private_data,
	                                      struct focalfp_dev, miscdev);
	int ret = 0;

	switch (cmd) {
	case FF_IOC_ENABLE_IRQ:
		spin_lock_irq(&fp->irq_lock);
		atomic_set(&fp->irq_fired, 0);
		if (!fp->irq_enabled) {
			fp->irq_enabled = true;
			enable_irq(fp->irq);
		}
		spin_unlock_irq(&fp->irq_lock);

		ret = wait_event_interruptible(fp->irq_wq,
		                               atomic_read(&fp->irq_fired));
		if (ret == 0)
			__pm_relax(fp->ws);
		break;

	case FF_IOC_DISABLE_IRQ:
		spin_lock_irq(&fp->irq_lock);
		if (fp->irq_enabled) {
			disable_irq_nosync(fp->irq);
			fp->irq_enabled = false;
		}
		atomic_set(&fp->irq_fired, 1);
		wake_up_interruptible(&fp->irq_wq);
		spin_unlock_irq(&fp->irq_lock);
		__pm_relax(fp->ws);
		break;

	case FF_IOC_RESET_DEVICE:
		ret = focalfp_do_reset(fp);
		break;

	default:
		ret = -ENOTTY;
	}
	return ret;
}

static int focalfp_release(struct inode *inode, struct file *file)
{
	struct focalfp_dev *fp = container_of(file->private_data,
	                                      struct focalfp_dev, miscdev);
	/* Release wakeup source if process exits while blocked in ENABLE_IRQ */
	__pm_relax(fp->ws);
	return 0;
}

static const struct file_operations focalfp_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = focalfp_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	.release        = focalfp_release,
};

static int focalfp_probe(struct platform_device *pdev)
{
	struct focalfp_dev *fp;
	int irq, ret;

	fp = devm_kzalloc(&pdev->dev, sizeof(*fp), GFP_KERNEL);
	if (!fp)
		return -ENOMEM;

	init_waitqueue_head(&fp->irq_wq);
	spin_lock_init(&fp->irq_lock);
	atomic_set(&fp->irq_fired, 0);

	fp->ws = wakeup_source_register(&pdev->dev, "focaltech_fp");
	if (!fp->ws)
		return -ENOMEM;

	fp->pinctrl = devm_pinctrl_get(&pdev->dev);
	if (IS_ERR(fp->pinctrl)) {
		ret = dev_err_probe(&pdev->dev, PTR_ERR(fp->pinctrl),
		                    "failed to get pinctrl\n");
		goto err_ws;
	}

#define GET_STATE(field, name) \
	fp->field = pinctrl_lookup_state(fp->pinctrl, name); \
	if (IS_ERR(fp->field)) { \
		ret = dev_err_probe(&pdev->dev, PTR_ERR(fp->field), \
		                    "pinctrl state '" name "' not found\n"); \
		goto err_ws; \
	}

	GET_STATE(st_rst_low,  "fpsensor_finger_rst_low");
	GET_STATE(st_rst_high, "fpsensor_finger_rst_high");
	GET_STATE(st_irq,      "fpsensor_eint_as_int");
	GET_STATE(st_pwr_low,  "fpsensor_finger_power_low");
	GET_STATE(st_pwr_high, "fpsensor_finger_power_high");
#undef GET_STATE

	/* Power on */
	ret = pinctrl_select_state(fp->pinctrl, fp->st_pwr_high);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "VDD on failed\n");
		goto err_ws;
	}
	msleep(10);

	/* Reset sequence */
	ret = focalfp_do_reset(fp);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "reset failed\n");
		goto err_pwr;
	}

	/* Configure IRQ pin */
	ret = pinctrl_select_state(fp->pinctrl, fp->st_irq);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "IRQ pinctrl failed\n");
		goto err_pwr;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		ret = irq;
		goto err_pwr;
	}
	fp->irq = irq;

	ret = devm_request_irq(&pdev->dev, irq, focalfp_irq_handler,
	                        IRQF_TRIGGER_RISING | IRQF_NO_AUTOEN,
	                        "focaltech_fp", fp);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "IRQ request failed\n");
		goto err_pwr;
	}

	/* Clear any edge the GIC latched during sensor power-on/reset */
	irq_set_irqchip_state(fp->irq, IRQCHIP_STATE_PENDING, false);
	fp->irq_enabled = false;

	fp->miscdev.minor  = MISC_DYNAMIC_MINOR;
	fp->miscdev.name   = "focaltech_fp";
	fp->miscdev.fops   = &focalfp_fops;
	fp->miscdev.parent = &pdev->dev;

	ret = misc_register(&fp->miscdev);
	if (ret) {
		dev_err_probe(&pdev->dev, ret, "misc_register failed\n");
		goto err_pwr;
	}

	platform_set_drvdata(pdev, fp);
	dev_info(&pdev->dev, "FocalTech FT9362 fingerprint driver loaded\n");
	return 0;

err_pwr:
	pinctrl_select_state(fp->pinctrl, fp->st_pwr_low);
err_ws:
	wakeup_source_unregister(fp->ws);
	return ret;
}

static void focalfp_remove(struct platform_device *pdev)
{
	struct focalfp_dev *fp = platform_get_drvdata(pdev);
	int ret;

	misc_deregister(&fp->miscdev);
	wakeup_source_unregister(fp->ws);
	ret = pinctrl_select_state(fp->pinctrl, fp->st_pwr_low);
	if (ret)
		dev_warn(&pdev->dev, "VDD power-down failed: %d\n", ret);
}

static const struct of_device_id focalfp_of_match[] = {
	{ .compatible = "focaltech,ft9362" },
	{}
};
MODULE_DEVICE_TABLE(of, focalfp_of_match);

static struct platform_driver focalfp_driver = {
	.probe  = focalfp_probe,
	.remove = focalfp_remove,
	.driver = {
		.name           = "focaltech-fp",
		.of_match_table = focalfp_of_match,
	},
};
module_platform_driver(focalfp_driver);

MODULE_AUTHOR("Marcus Ramberg <marcus.ramberg@gmail.com>");
MODULE_DESCRIPTION("FocalTech FT9362 fingerprint sensor GPIO/IRQ driver");
MODULE_LICENSE("GPL");
