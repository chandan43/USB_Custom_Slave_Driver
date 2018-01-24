#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/scatterlist.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/usb.h>

#define SIMPLE_IO_TIMEOUT	10000	/* in milliseconds */

/*-------------------------------------------------------------------------*/

static int override_alt = -1;
module_param_named(alt, override_alt, int, 0644);
MODULE_PARM_DESC(alt, ">= 0 to override altsetting selection");

/*-------------------------------------------------------------------------*/

/* FIXME make these public somewhere; usbdevfs.h? */
struct usbtest_param {
	/* inputs */
	unsigned		test_num;	/* 0..(TEST_CASES-1) */
	unsigned		iterations;
	unsigned		length;
	unsigned		vary;
	unsigned		sglen;

	/* outputs */
	struct timeval		duration;
};
#define USBTEST_REQUEST	_IOWR('U', 100, struct usbtest_param)

/*-------------------------------------------------------------------------*/

#define	GENERIC		/* let probe() bind using module params */

/* Some devices that can be used for testing will have "real" drivers.
 * Entries for those need to be enabled here by hand, after disabling
 * that "real" driver.
 */
//#define	IBOT2		/* grab iBOT2 webcams */
//#define	KEYSPAN_19Qi	/* grab un-renumerated serial adapter */

/*-------------------------------------------------------------------------*/

struct usbtest_info {
	const char		*name;
	u8			ep_in;		/* bulk/intr source */
	u8			ep_out;		/* bulk/intr sink */
	unsigned		autoconf:1;
	unsigned		ctrl_out:1;
	unsigned		iso:1;		/* try iso in/out */
	unsigned		intr:1;		/* try interrupt in/out */
	int			alt;
};

/* this is accessed only through usbfs ioctl calls.
 * one ioctl to issue a test ... one lock per device.
 * tests create other threads if they need them.
 * urbs and buffers are allocated dynamically,
 * and data generated deterministically.
 */
struct usbtest_dev {
	struct usb_interface	*intf;
	struct usbtest_info	*info;
	int			in_pipe;
	int			out_pipe;
	int			in_iso_pipe;
	int			out_iso_pipe;
	int			in_int_pipe;
	int			out_int_pipe;
	struct usb_endpoint_descriptor	*iso_in, *iso_out;
	struct usb_endpoint_descriptor	*int_in, *int_out;
	struct mutex		lock;

#define TBUF_SIZE	256
	u8			*buf;
};

static struct usb_device *testdev_to_usbdev(struct usbtest_dev *test)
{
	return interface_to_usbdev(test->intf);
}

/* set up all urbs so they can be used with either bulk or interrupt */
#define	INTERRUPT_RATE		1	/* msec/transfer */

#define ERROR(tdev, fmt, args...) \
	dev_err(&(tdev)->intf->dev , fmt , ## args)
#define WARNING(tdev, fmt, args...) \
	dev_warn(&(tdev)->intf->dev , fmt , ## args)

#define GUARD_BYTE	0xA5
#define MAX_SGLEN	128

/*-------------------------------------------------------------------------*/

static int
get_endpoints(struct usbtest_dev *dev, struct usb_interface *intf)
{
	int				tmp;
	struct usb_host_interface	*alt;
	struct usb_host_endpoint	*in, *out;
	struct usb_host_endpoint	*iso_in, *iso_out;
	struct usb_host_endpoint	*int_in, *int_out;
	struct usb_device		*udev;

	for (tmp = 0; tmp < intf->num_altsetting; tmp++) {
		unsigned	ep;

		in = out = NULL;
		iso_in = iso_out = NULL;
		int_in = int_out = NULL;
		alt = intf->altsetting + tmp;

		if (override_alt >= 0 &&
				override_alt != alt->desc.bAlternateSetting)
			continue;

		/* take the first altsetting with in-bulk + out-bulk;
		 * ignore other endpoints and altsettings.
		 */
		for (ep = 0; ep < alt->desc.bNumEndpoints; ep++) {
			struct usb_host_endpoint	*e;

			e = alt->endpoint + ep;
			switch (usb_endpoint_type(&e->desc)) {
			case USB_ENDPOINT_XFER_BULK:
				break;
			case USB_ENDPOINT_XFER_INT:
				if (dev->info->intr)
					goto try_intr;
				continue;
			case USB_ENDPOINT_XFER_ISOC:
				if (dev->info->iso)
					goto try_iso;
				/* FALLTHROUGH */
			default:
				continue;
			}
			if (usb_endpoint_dir_in(&e->desc)) {
				if (!in)
					in = e;
			} else {
				if (!out)
					out = e;
			}
			continue;
try_intr:
			if (usb_endpoint_dir_in(&e->desc)) {
				if (!int_in)
					int_in = e;
			} else {
				if (!int_out)
					int_out = e;
			}
			continue;
try_iso:
			if (usb_endpoint_dir_in(&e->desc)) {
				if (!iso_in)
					iso_in = e;
			} else {
				if (!iso_out)
					iso_out = e;
			}
		}
		if ((in && out)  ||  iso_in || iso_out || int_in || int_out)
			goto found;
	}
	return -EINVAL;

found:
	udev = testdev_to_usbdev(dev);
	dev->info->alt = alt->desc.bAlternateSetting;
	if (alt->desc.bAlternateSetting != 0) {
		tmp = usb_set_interface(udev,
				alt->desc.bInterfaceNumber,
				alt->desc.bAlternateSetting);
		if (tmp < 0)
			return tmp;
	}

	if (in) {
		dev->in_pipe = usb_rcvbulkpipe(udev,
			in->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
		dev->out_pipe = usb_sndbulkpipe(udev,
			out->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
	}
	if (iso_in) {
		dev->iso_in = &iso_in->desc;
		dev->in_iso_pipe = usb_rcvisocpipe(udev,
				iso_in->desc.bEndpointAddress
					& USB_ENDPOINT_NUMBER_MASK);
	}

	if (iso_out) {
		dev->iso_out = &iso_out->desc;
		dev->out_iso_pipe = usb_sndisocpipe(udev,
				iso_out->desc.bEndpointAddress
					& USB_ENDPOINT_NUMBER_MASK);
	}

	if (int_in) {
		dev->int_in = &int_in->desc;
		dev->in_int_pipe = usb_rcvintpipe(udev,
				int_in->desc.bEndpointAddress
					& USB_ENDPOINT_NUMBER_MASK);
	}

	if (int_out) {
		dev->int_out = &int_out->desc;
		dev->out_int_pipe = usb_sndintpipe(udev,
				int_out->desc.bEndpointAddress
					& USB_ENDPOINT_NUMBER_MASK);
	}
	return 0;
}

/*-------------------------------------------------------------------------*/

/* unqueued control message testing
 *
 * there's a nice set of device functional requirements in chapter 9 of the
 * usb 2.0 spec, which we can apply to ANY device, even ones that don't use
 * special test firmware.
 *
 * we know the device is configured (or suspended) by the time it's visible
 * through usbfs.  we can't change that, so we won't test enumeration (which
 * worked 'well enough' to get here, this time), power management (ditto),
 * or remote wakeup (which needs human interaction).
 */

static unsigned realworld = 1;
module_param(realworld, uint, 0);
MODULE_PARM_DESC(realworld, "clear to demand stricter spec compliance");

static int set_altsetting(struct usbtest_dev *dev, int alternate)
{
	struct usb_interface		*iface = dev->intf;
	struct usb_device		*udev;

	if (alternate < 0 || alternate >= 256)
		return -EINVAL;

	udev = interface_to_usbdev(iface);
	return usb_set_interface(udev,
			iface->altsetting[0].desc.bInterfaceNumber,
			alternate);
}

static unsigned pattern;
static unsigned mod_pattern;
module_param_named(pattern, mod_pattern, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(mod_pattern, "i/o pattern (0 == zeroes)");
/*-------------------------------------------------------------------------*/

/* Control OUT tests use the vendor control requests from Intel's
 * USB 2.0 compliance test device:  write a buffer, read it back.
 *
 * Intel's spec only _requires_ that it work for one packet, which
 * is pretty weak.   Some HCDs place limits here; most devices will
 * need to be able to handle more than one OUT data packet.  We'll
 * try whatever we're told to try.
 */
static int ctrl_out(struct usbtest_dev *dev,
		unsigned count, unsigned length, unsigned vary, unsigned offset)
{
	unsigned		i, j, len;
	int			retval;
	u8			*buf;
	char			*what = "?";
	struct usb_device	*udev;

	if (length < 1 || length > 0xffff || vary >= length)
		return -EINVAL;

	buf = kmalloc(length + offset, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf += offset;
	udev = testdev_to_usbdev(dev);
	len = length;
	retval = 0;

	/* NOTE:  hardware might well act differently if we pushed it
	 * with lots back-to-back queued requests.
	 */
	for (i = 0; i < count; i++) {
		/* write patterned data */
		for (j = 0; j < len; j++)
			buf[j] = (u8)(i + j);
		retval = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
				0x5b, USB_DIR_OUT|USB_TYPE_VENDOR,
				0, 0, buf, len, USB_CTRL_SET_TIMEOUT);
		if (retval != len) {
			what = "write";
			if (retval >= 0) {
				ERROR(dev, "ctrl_out, wlen %d (expected %d)\n",
						retval, len);
				retval = -EBADMSG;
			}
			break;
		}

		/* read it back -- assuming nothing intervened!!  */
		retval = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
				0x5c, USB_DIR_IN|USB_TYPE_VENDOR,
				0, 0, buf, len, USB_CTRL_GET_TIMEOUT);
		if (retval != len) {
			what = "read";
			if (retval >= 0) {
				ERROR(dev, "ctrl_out, rlen %d (expected %d)\n",
						retval, len);
				retval = -EBADMSG;
			}
			break;
		}

		/* fail if we can't verify */
		for (j = 0; j < len; j++) {
			if (buf[j] != (u8)(i + j)) {
				ERROR(dev, "ctrl_out, byte %d is %d not %d\n",
					j, buf[j], (u8)(i + j));
				retval = -EBADMSG;
				break;
			}
		}
		if (retval < 0) {
			what = "verify";
			break;
		}

		len += vary;

		/* [real world] the "zero bytes IN" case isn't really used.
		 * hardware can easily trip up in this weird case, since its
		 * status stage is IN, not OUT like other ep0in transfers.
		 */
		if (len > length)
			len = realworld ? 1 : 0;
	}

	if (retval < 0)
		ERROR(dev, "ctrl_out %s failed, code %d, count %d\n",
			what, retval, i);

	kfree(buf - offset);
	return retval;
}

/*-------------------------------------------------------------------------*/

/* We only have this one interface to user space, through usbfs.
 * User mode code can scan usbfs to find N different devices (maybe on
 * different busses) to use when testing, and allocate one thread per
 * test.  So discovery is simplified, and we have no device naming issues.
 *
 * Don't use these only as stress/load tests.  Use them along with with
 * other USB bus activity:  plugging, unplugging, mousing, mp3 playback,
 * video capture, and so on.  Run different tests at different times, in
 * different sequences.  Nothing here should interact with other devices,
 * except indirectly by consuming USB bandwidth and CPU resources for test
 * threads and request completion.  But the only way to know that for sure
 * is to test when HC queues are in use by many devices.
 *
 * WARNING:  Because usbfs grabs udev->dev.sem before calling this ioctl(),
 * it locks out usbcore in certain code paths.  Notably, if you disconnect
 * the device-under-test, hub_wq will wait block forever waiting for the
 * ioctl to complete ... so that usb_disconnect() can abort the pending
 * urbs and then call usbtest_disconnect().  To abort a test, you're best
 * off just killing the userspace task and waiting for it to exit.
 */

static int
usbtest_ioctl(struct usb_interface *intf, unsigned int code, void *buf)
{
	struct usbtest_dev	*dev = usb_get_intfdata(intf);
//	struct usb_device	*udev = testdev_to_usbdev(dev);
	struct usbtest_param	*param = buf;
	int			retval = -EOPNOTSUPP;
//	struct urb		*urb;
//	struct scatterlist	*sg;
//	struct usb_sg_request	req;
	struct timeval		start;
//	unsigned		i;

	/* FIXME USBDEVFS_CONNECTINFO doesn't say how fast the device is. */

	pattern = mod_pattern;

	if (code != USBTEST_REQUEST)
		return -EOPNOTSUPP;

	if (param->iterations <= 0)
		return -EINVAL;

	if (param->sglen > MAX_SGLEN)
		return -EINVAL;

	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	/* FIXME: What if a system sleep starts while a test is running? */

	/* some devices, like ez-usb default devices, need a non-default
	 * altsetting to have any active endpoints.  some tests change
	 * altsettings; force a default so most tests don't need to check.
	 */
	if (dev->info->alt >= 0) {
		int	res;

		if (intf->altsetting->desc.bInterfaceNumber) {
			mutex_unlock(&dev->lock);
			return -ENODEV;
		}
		res = set_altsetting(dev, dev->info->alt);
		if (res) {
			dev_err(&intf->dev,
					"set altsetting to %d failed, %d\n",
					dev->info->alt, res);
			mutex_unlock(&dev->lock);
			return res;
		}
	}

	/*
	 * Just a bunch of test cases that every HCD is expected to handle.
	 *
	 * Some may need specific firmware, though it'd be good to have
	 * one firmware image to handle all the test cases.
	 *
	 * FIXME add more tests!  cancel requests, verify the data, control
	 * queueing, concurrent read+write threads, and so on.
	 */
	do_gettimeofday(&start);
	switch (param->test_num) {

	case 0:
		dev_info(&intf->dev, "TEST 0:  NOP\n");
		retval = 0;
		break;

	/* Simple non-queued bulk I/O tests */

	/* control write tests */
	case 14:
		if (!dev->info->ctrl_out)
			break;
		dev_info(&intf->dev, "TEST 14:  %d ep0out, %d..%d vary %d\n",
				param->iterations,
				realworld ? 1 : 0, param->length,
				param->vary);
		retval = ctrl_out(dev, param->iterations,
				param->length, param->vary, 0);
		break;

	}
	do_gettimeofday(&param->duration);
	param->duration.tv_sec -= start.tv_sec;
	param->duration.tv_usec -= start.tv_usec;
	if (param->duration.tv_usec < 0) {
		param->duration.tv_usec += 1000 * 1000;
		param->duration.tv_sec -= 1;
	}
	mutex_unlock(&dev->lock);
	return retval;
}

/*-------------------------------------------------------------------------*/

static unsigned force_interrupt;
module_param(force_interrupt, uint, 0);
MODULE_PARM_DESC(force_interrupt, "0 = test default; else interrupt");

#ifdef	GENERIC
static unsigned short vendor;
module_param(vendor, ushort, 0);
MODULE_PARM_DESC(vendor, "vendor code (from usb-if)");

static unsigned short product;
module_param(product, ushort, 0);
MODULE_PARM_DESC(product, "product code (from vendor)");
#endif

static int
usbtest_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device	*udev;
	struct usbtest_dev	*dev;
	struct usbtest_info	*info;
	char			*rtest, *wtest;
	char			*irtest, *iwtest;
	char			*intrtest, *intwtest;

	udev = interface_to_usbdev(intf);

#ifdef	GENERIC
	/* specify devices by module parameters? */
	if (id->match_flags == 0) {
		/* vendor match required, product match optional */
		if (!vendor || le16_to_cpu(udev->descriptor.idVendor) != (u16)vendor)
			return -ENODEV;
		if (product && le16_to_cpu(udev->descriptor.idProduct) != (u16)product)
			return -ENODEV;
		dev_info(&intf->dev, "matched module params, "
					"vend=0x%04x prod=0x%04x\n",
				le16_to_cpu(udev->descriptor.idVendor),
				le16_to_cpu(udev->descriptor.idProduct));
	}
#endif

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	info = (struct usbtest_info *) id->driver_info;
	dev->info = info;
	mutex_init(&dev->lock);

	dev->intf = intf;

	/* cacheline-aligned scratch for i/o */
	dev->buf = kmalloc(TBUF_SIZE, GFP_KERNEL);
	if (dev->buf == NULL) {
		kfree(dev);
		return -ENOMEM;
	}

	/* NOTE this doesn't yet test the handful of difference that are
	 * visible with high speed interrupts:  bigger maxpacket (1K) and
	 * "high bandwidth" modes (up to 3 packets/uframe).
	 */
	rtest = wtest = "";
	irtest = iwtest = "";
	intrtest = intwtest = "";
	if (force_interrupt || udev->speed == USB_SPEED_LOW) {
		if (info->ep_in) {
			dev->in_pipe = usb_rcvintpipe(udev, info->ep_in);
			rtest = " intr-in";
		}
		if (info->ep_out) {
			dev->out_pipe = usb_sndintpipe(udev, info->ep_out);
			wtest = " intr-out";
		}
	} else {
		if (override_alt >= 0 || info->autoconf) {
			int status;

			status = get_endpoints(dev, intf);
			if (status < 0) {
				WARNING(dev, "couldn't get endpoints, %d\n",
						status);
				kfree(dev->buf);
				kfree(dev);
				return status;
			}
			/* may find bulk or ISO pipes */
		} else {
			if (info->ep_in)
				dev->in_pipe = usb_rcvbulkpipe(udev,
							info->ep_in);
			if (info->ep_out)
				dev->out_pipe = usb_sndbulkpipe(udev,
							info->ep_out);
		}
		if (dev->in_pipe)
			rtest = " bulk-in";
		if (dev->out_pipe)
			wtest = " bulk-out";
		if (dev->in_iso_pipe)
			irtest = " iso-in";
		if (dev->out_iso_pipe)
			iwtest = " iso-out";
		if (dev->in_int_pipe)
			intrtest = " int-in";
		if (dev->out_int_pipe)
			intwtest = " int-out";
	}

	usb_set_intfdata(intf, dev);
	dev_info(&intf->dev, "%s\n", info->name);
	dev_info(&intf->dev, "%s {control%s%s%s%s%s%s%s} tests%s\n",
			usb_speed_string(udev->speed),
			info->ctrl_out ? " in/out" : "",
			rtest, wtest,
			irtest, iwtest,
			intrtest, intwtest,
			info->alt >= 0 ? " (+alt)" : "");
	return 0;
}

static int usbtest_suspend(struct usb_interface *intf, pm_message_t message)
{
	return 0;
}

static int usbtest_resume(struct usb_interface *intf)
{
	return 0;
}


static void usbtest_disconnect(struct usb_interface *intf)
{
	struct usbtest_dev	*dev = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	dev_dbg(&intf->dev, "disconnect\n");
	kfree(dev);
}

/* Basic testing only needs a device that can source or sink bulk traffic.
 * Any device can test control transfers (default with GENERIC binding).
 *
 * Several entries work with the default EP0 implementation that's built
 * into EZ-USB chips.  There's a default vendor ID which can be overridden
 * by (very) small config EEPROMS, but otherwise all these devices act
 * identically until firmware is loaded:  only EP0 works.  It turns out
 * to be easy to make other endpoints work, without modifying that EP0
 * behavior.  For now, we expect that kind of firmware.
 */

/* peripheral running Linux and 'zero.c' test firmware, or
 * its user-mode cousin. different versions of this use
 * different hardware with the same vendor/product codes.
 * host side MUST rely on the endpoint descriptors.
 */
static struct usbtest_info gz_info = {
	.name		= "Linux gadget zero",
	.autoconf	= 1,
	.ctrl_out	= 1,
	.iso		= 1,
	.intr		= 1,
	.alt		= 0,
};


static const struct usb_device_id id_table[] = {

	/*-------------------------------------------------------------*/
	/* "Gadget Zero" firmware runs under Linux */
	{ USB_DEVICE(0x0525, 0xa4a0),
		.driver_info = (unsigned long) &gz_info,
	},
	/*-------------------------------------------------------------*/

	{ }
};
MODULE_DEVICE_TABLE(usb, id_table);

static struct usb_driver usbtest_driver = {
	.name =		"usbtest",
	.id_table =	id_table,
	.probe =	usbtest_probe,
	.unlocked_ioctl = usbtest_ioctl,
	.disconnect =	usbtest_disconnect,
	.suspend =	usbtest_suspend,
	.resume =	usbtest_resume,
};

/*-------------------------------------------------------------------------*/

static int __init usbtest_init(void)
{
#ifdef GENERIC
	if (vendor)
		pr_debug("params: vend=0x%04x prod=0x%04x\n", vendor, product);
#endif
	return usb_register(&usbtest_driver);
}
module_init(usbtest_init);

static void __exit usbtest_exit(void)
{
	usb_deregister(&usbtest_driver);
}
module_exit(usbtest_exit);

MODULE_DESCRIPTION("USB Core/HCD Testing Driver");
MODULE_LICENSE("GPL");

