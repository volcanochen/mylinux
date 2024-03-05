// SPDX-License-Identifier: GPL-2.0
/*
 * Defines interfaces for interacting wtih the Raspberry Pi firmware's
 * property channel.
 *
 * Copyright © 2015 Broadcom
 */

#include <linux/dma-mapping.h>
#include <linux/mailbox_client.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <soc/bcm2835/raspberrypi-firmware.h>

#define MBOX_MSG(chan, data28)		(((data28) & ~0xf) | ((chan) & 0xf))
#define MBOX_CHAN(msg)			((msg) & 0xf)
#define MBOX_DATA28(msg)		((msg) & ~0xf)
#define MBOX_CHAN_PROPERTY		8

#define VL805_PCI_CONFIG_VERSION_OFFSET		0x50

static struct platform_device *rpi_hwmon;
static struct platform_device *rpi_clk;

struct rpi_firmware {
	struct mbox_client cl;
	struct mbox_chan *chan; /* The property channel. */
	struct completion c;
	u32 enabled;
};

static DEFINE_MUTEX(transaction_lock);

static void response_callback(struct mbox_client *cl, void *msg)
{
	struct rpi_firmware *fw = container_of(cl, struct rpi_firmware, cl);
	complete(&fw->c);
}

/*
 * Sends a request to the firmware through the BCM2835 mailbox driver,
 * and synchronously waits for the reply.
 */
static int
rpi_firmware_transaction(struct rpi_firmware *fw, u32 chan, u32 data)
{
	u32 message = MBOX_MSG(chan, data);
	int ret;

	WARN_ON(data & 0xf);

	mutex_lock(&transaction_lock);
	reinit_completion(&fw->c);
	ret = mbox_send_message(fw->chan, &message);
	if (ret >= 0) {
		if (wait_for_completion_timeout(&fw->c, HZ)) {
			ret = 0;
		} else {
			ret = -ETIMEDOUT;
			WARN_ONCE(1, "Firmware transaction timeout");
		}
	} else {
		dev_err(fw->cl.dev, "mbox_send_message returned %d\n", ret);
	}
	mutex_unlock(&transaction_lock);

	return ret;
}

/**
 * rpi_firmware_property_list - Submit firmware property list
 * @fw:		Pointer to firmware structure from rpi_firmware_get().
 * @data:	Buffer holding tags.
 * @tag_size:	Size of tags buffer.
 *
 * Submits a set of concatenated tags to the VPU firmware through the
 * mailbox property interface.
 *
 * The buffer header and the ending tag are added by this function and
 * don't need to be supplied, just the actual tags for your operation.
 * See struct rpi_firmware_property_tag_header for the per-tag
 * structure.
 */
int rpi_firmware_property_list(struct rpi_firmware *fw,
			       void *data, size_t tag_size)
{
	size_t size = tag_size + 12;
	u32 *buf;
	dma_addr_t bus_addr;
	int ret;

	/* Packets are processed a dword at a time. */
	if (size & 3)
		return -EINVAL;

	buf = dma_alloc_coherent(fw->cl.dev, PAGE_ALIGN(size), &bus_addr,
				 GFP_ATOMIC);
	if (!buf)
		return -ENOMEM;

	/* The firmware will error out without parsing in this case. */
	WARN_ON(size >= 1024 * 1024);

	buf[0] = size;
	buf[1] = RPI_FIRMWARE_STATUS_REQUEST;
	memcpy(&buf[2], data, tag_size);
	buf[size / 4 - 1] = RPI_FIRMWARE_PROPERTY_END;
	wmb();

	ret = rpi_firmware_transaction(fw, MBOX_CHAN_PROPERTY, bus_addr);

	rmb();
	memcpy(data, &buf[2], tag_size);
	if (ret == 0 && buf[1] != RPI_FIRMWARE_STATUS_SUCCESS) {
		/*
		 * The tag name here might not be the one causing the
		 * error, if there were multiple tags in the request.
		 * But single-tag is the most common, so go with it.
		 */
		dev_err(fw->cl.dev, "Request 0x%08x returned status 0x%08x\n",
			buf[2], buf[1]);
		ret = -EINVAL;
	}

	dma_free_coherent(fw->cl.dev, PAGE_ALIGN(size), buf, bus_addr);

	return ret;
}
EXPORT_SYMBOL_GPL(rpi_firmware_property_list);

/**
 * rpi_firmware_property - Submit single firmware property
 * @fw:		Pointer to firmware structure from rpi_firmware_get().
 * @tag:	One of enum_mbox_property_tag.
 * @tag_data:	Tag data buffer.
 * @buf_size:	Buffer size.
 *
 * Submits a single tag to the VPU firmware through the mailbox
 * property interface.
 *
 * This is a convenience wrapper around
 * rpi_firmware_property_list() to avoid some of the
 * boilerplate in property calls.
 */
int rpi_firmware_property(struct rpi_firmware *fw,
			  u32 tag, void *tag_data, size_t buf_size)
{
	struct rpi_firmware_property_tag_header *header;
	int ret;

	/* Some mailboxes can use over 1k bytes. Rather than checking
	 * size and using stack or kmalloc depending on requirements,
	 * just use kmalloc. Mailboxes don't get called enough to worry
	 * too much about the time taken in the allocation.
	 */
	void *data = kmalloc(sizeof(*header) + buf_size, GFP_KERNEL);

	if (!data)
		return -ENOMEM;

	header = data;
	header->tag = tag;
	header->buf_size = buf_size;
	header->req_resp_size = 0;
	memcpy(data + sizeof(*header), tag_data, buf_size);

	ret = rpi_firmware_property_list(fw, data, buf_size + sizeof(*header));

	memcpy(tag_data, data + sizeof(*header), buf_size);

	kfree(data);

	return ret;
}
EXPORT_SYMBOL_GPL(rpi_firmware_property);

static void
rpi_firmware_print_firmware_revision(struct rpi_firmware *fw)
{
	time64_t date_and_time;
	u32 packet;
	int ret = rpi_firmware_property(fw,
					RPI_FIRMWARE_GET_FIRMWARE_REVISION,
					&packet, sizeof(packet));

	if (ret)
		return;

	/* This is not compatible with y2038 */
	date_and_time = packet;
	dev_info(fw->cl.dev, "Attached to firmware from %ptT\n", &date_and_time);
}

static void
rpi_register_hwmon_driver(struct device *dev, struct rpi_firmware *fw)
{
	u32 packet;
	int ret = rpi_firmware_property(fw, RPI_FIRMWARE_GET_THROTTLED,
					&packet, sizeof(packet));

	if (ret)
		return;

	rpi_hwmon = platform_device_register_data(dev, "raspberrypi-hwmon",
						  -1, NULL, 0);
}

static void rpi_register_clk_driver(struct device *dev)
{
	struct device_node *firmware;

	/*
	 * Earlier DTs don't have a node for the firmware clocks but
	 * rely on us creating a platform device by hand. If we do
	 * have a node for the firmware clocks, just bail out here.
	 */
	firmware = of_get_compatible_child(dev->of_node,
					   "raspberrypi,firmware-clocks");
	if (firmware) {
		of_node_put(firmware);
		return;
	}

	rpi_clk = platform_device_register_data(dev, "raspberrypi-clk",
						-1, NULL, 0);
}

static int rpi_firmware_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct rpi_firmware *fw;

	fw = devm_kzalloc(dev, sizeof(*fw), GFP_KERNEL);
	if (!fw)
		return -ENOMEM;

	fw->cl.dev = dev;
	fw->cl.rx_callback = response_callback;
	fw->cl.tx_block = true;

	fw->chan = mbox_request_channel(&fw->cl, 0);
	if (IS_ERR(fw->chan)) {
		int ret = PTR_ERR(fw->chan);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to get mbox channel: %d\n", ret);
		return ret;
	}

	init_completion(&fw->c);

	platform_set_drvdata(pdev, fw);

	rpi_firmware_print_firmware_revision(fw);
	rpi_register_hwmon_driver(dev, fw);
	rpi_register_clk_driver(dev);

	return 0;
}

static void rpi_firmware_shutdown(struct platform_device *pdev)
{
	struct rpi_firmware *fw = platform_get_drvdata(pdev);

	if (!fw)
		return;

	rpi_firmware_property(fw, RPI_FIRMWARE_NOTIFY_REBOOT, NULL, 0);
}

static int rpi_firmware_remove(struct platform_device *pdev)
{
	struct rpi_firmware *fw = platform_get_drvdata(pdev);

	platform_device_unregister(rpi_hwmon);
	rpi_hwmon = NULL;
	platform_device_unregister(rpi_clk);
	rpi_clk = NULL;
	mbox_free_channel(fw->chan);

	return 0;
}

/**
 * rpi_firmware_get - Get pointer to rpi_firmware structure.
 * @firmware_node:    Pointer to the firmware Device Tree node.
 *
 * Returns NULL is the firmware device is not ready.
 */
struct rpi_firmware *rpi_firmware_get(struct device_node *firmware_node)
{
	struct platform_device *pdev = of_find_device_by_node(firmware_node);

	if (!pdev)
		return NULL;

	return platform_get_drvdata(pdev);
}
EXPORT_SYMBOL_GPL(rpi_firmware_get);

/*
 * The Raspberry Pi 4 gets its USB functionality from VL805, a PCIe chip that
 * implements xHCI. After a PCI reset, VL805's firmware may either be loaded
 * directly from an EEPROM or, if not present, by the SoC's co-processor,
 * VideoCore. RPi4's VideoCore OS contains both the non public firmware load
 * logic and the VL805 firmware blob. This function triggers the aforementioned
 * process.
 */
int rpi_firmware_init_vl805(struct pci_dev *pdev)
{
	struct device_node *fw_np;
	struct rpi_firmware *fw;
	u32 dev_addr, version;
	int ret;

	fw_np = of_find_compatible_node(NULL, NULL,
					"raspberrypi,bcm2835-firmware");
	if (!fw_np)
		return 0;

	fw = rpi_firmware_get(fw_np);
	of_node_put(fw_np);
	if (!fw)
		return -ENODEV;

	/*
	 * Make sure we don't trigger a firmware load unnecessarily.
	 *
	 * If something went wrong with PCI, this whole exercise would be
	 * futile as VideoCore expects from us a configured PCI bus. Just take
	 * the faulty version (likely ~0) and let xHCI's registration fail
	 * further down the line.
	 */
	pci_read_config_dword(pdev, VL805_PCI_CONFIG_VERSION_OFFSET, &version);
	if (version)
		goto exit;

	dev_addr = pdev->bus->number << 20 | PCI_SLOT(pdev->devfn) << 15 |
		   PCI_FUNC(pdev->devfn) << 12;

	ret = rpi_firmware_property(fw, RPI_FIRMWARE_NOTIFY_XHCI_RESET,
				    &dev_addr, sizeof(dev_addr));
	if (ret)
		return ret;

	/* Wait for vl805 to startup */
	usleep_range(200, 1000);

	pci_read_config_dword(pdev, VL805_PCI_CONFIG_VERSION_OFFSET,
			      &version);
exit:
	pci_info(pdev, "VL805 firmware version %08x\n", version);

	return 0;
}
EXPORT_SYMBOL_GPL(rpi_firmware_init_vl805);

static const struct of_device_id rpi_firmware_of_match[] = {
	{ .compatible = "raspberrypi,bcm2835-firmware", },
	{},
};
MODULE_DEVICE_TABLE(of, rpi_firmware_of_match);

static struct platform_driver rpi_firmware_driver = {
	.driver = {
		.name = "raspberrypi-firmware",
		.of_match_table = rpi_firmware_of_match,
	},
	.probe		= rpi_firmware_probe,
	.shutdown	= rpi_firmware_shutdown,
	.remove		= rpi_firmware_remove,
};
module_platform_driver(rpi_firmware_driver);

MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_DESCRIPTION("Raspberry Pi firmware driver");
MODULE_LICENSE("GPL v2");