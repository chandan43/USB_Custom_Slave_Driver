#include <linux/module.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/of_gpio.h>
#include <linux/kobject.h>

#define OMAP_GPIO_OE           0x0134
#define OMAP_GPIO_SETDATAOUT   0x0194
#define OMAP_GPIO_CLEARDATAOUT 0x0190
#define OMAP_GPIO_DATAIN       0x0138
#define LED_INIT __led_init()

struct gpiopin_bank {
        u32 gpio_base;
        unsigned int pinno;
};

static struct gpiopin_bank *gpbank;
static void *gbank_base;
static struct kobject *kobj;
static int ledinit;
/*
This will insert a barrier() so we know that the CPU will execute the
write of a before the write of b. However it does not mandate that
reg2 is written with b before reg1 is written with a at the hardware
register level. 
 
*/

static void __led_init(void)
{
	unsigned int data = 0;

	ledinit = 1;

	data = readl_relaxed(gbank_base + OMAP_GPIO_OE);
	data = data & 0xF7FFFFFF;
	pr_info("led_init: direction of pin is set: %x\n",data);
	writel_relaxed(data, gbank_base + OMAP_GPIO_OE);
	return;

}
static ssize_t led_read(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned int data;
	char rbuf;
	
	pr_info("extled_read: invoked from sysfs...\n");
	if (!ledinit)
		LED_INIT;
	data = readl_relaxed(gbank_base + OMAP_GPIO_DATAIN);
	data = data & (1U << gpbank->pinno) ? 1 : 0; /* 0 => off; 1 => on */
	if(data) {
		rbuf = 1;
		pr_info("extled_read: led is on...\n");
		return sprintf(buf,"%d\n",rbuf);
	}else {
		rbuf = 0;
		pr_info("extled_read: led is off...\n");
		return sprintf(buf,"%d\n",rbuf);
	}
	
}
void led_on(void)
{	
	unsigned int data;
	if (!ledinit)
		LED_INIT;
	data = readl_relaxed(gbank_base + OMAP_GPIO_DATAIN);
	pr_info("%s: Led is ON\n",__func__);
	data = data | (1U << gpbank->pinno);
	writel_relaxed(data, gbank_base + OMAP_GPIO_SETDATAOUT);
}
EXPORT_SYMBOL_GPL(led_on);
void led_off(void)
{
	unsigned int data;
	if (!ledinit)
		LED_INIT;
	data = readl_relaxed(gbank_base + OMAP_GPIO_DATAIN);
	pr_info("%s: Led is OFF\n",__func__);
	data = data | (1U << gpbank->pinno);
	writel_relaxed(data, gbank_base + OMAP_GPIO_CLEARDATAOUT);
}
EXPORT_SYMBOL_GPL(led_off);

static ssize_t led_write(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int data;
	int d;
	int ret;

	pr_info("%s: invoked from sysfs...\n",__func__);
	if (!ledinit)
		LED_INIT;
	ret=sscanf(buf,"%d",&d);
	if(ret < 0 || ret > 1 || d > 1)
		return -EINVAL;
	 data = readl_relaxed(gbank_base + OMAP_GPIO_DATAIN);
	if(d==0) {
		pr_info("%s: led is off\n",__func__);
		data = data | (1U << gpbank->pinno);
		writel_relaxed(data, gbank_base + OMAP_GPIO_CLEARDATAOUT);
	}else{
		pr_info("%s: led is on\n",__func__);
		data = data | (1U << gpbank->pinno);
		writel_relaxed(data, gbank_base + OMAP_GPIO_SETDATAOUT);
	}
	return data;
}
static struct kobj_attribute led_attr =__ATTR(extled, 0660, led_read, led_write);
/**
 * of_get_named_gpio() - Get a GPIO number to use with GPIO API
 * @np:		device node to get GPIO from
 * @propname:	Name of property containing gpio specifier(s)
 * @index:	index of the GPIO
 *
 * Returns GPIO number to use with Linux generic GPIO API, or one of the errno
 * value on the error condition.
 */
/*struct device_node *of_parse_phandle(const struct device_node *np, 
                                            const char *phandle_name,
                                            int index);
*/
/**
 * of_property_read_u32_u32 - Find and read a property.
 *
 * @np:         device node from which the property value is to be read.
 * @propname:   name of the property to be searched.
 * @out_values: pointer to return value, modified only if return value is 0.
 *
 *
 * Search for a property in a device node and read 32-bit value(s) from
 * it. Returns 0 on success, -EINVAL if the property does not exist,
 * -ENODATA if property does not have a value, and -EOVERFLOW if the
 * property data isn't large enough.
 *
 * The out_values is modified only if a valid u32 value can be decoded.
 */

static int onboardled_probe(struct platform_device *pdev)
{
	struct device_node *pnode;
	int ret;

	pr_info("%s: Invoked.!\n",__func__);

	gpbank = (struct gpiopin_bank *)kmalloc(sizeof(struct gpiopin_bank), GFP_KERNEL);
	/* Acess GPIO PIN number*/
	gpbank->pinno = of_get_named_gpio(pdev->dev.of_node, "gpios", 0);	
	pr_info("%s: GPIO pin no for led device: %u\n",__func__,gpbank->pinno);

	/* Acess node reffered by phandel*/
	pnode=of_parse_phandle(pdev->dev.of_node,"gpios",0);
	
	/*Accessing GPIO BASE address of register from phandel*/	
	ret = of_property_read_u32(pnode, "reg", &gpbank->gpio_base);	
	pr_info("%s: Gpio bank base addr: %x\n",__func__,gpbank->gpio_base);
	
	gbank_base = ioremap(gpbank->gpio_base, 4095); /* ioremap(unsigned long port, unsigned long size)*/	
	if(!gbank_base)
		return -1;

	pr_info("BASE_ADD: gbank_base address as seen by kernel: %p\n", gbank_base);
	
	kobj = kobject_create_and_add("led", NULL);
	if(!kobj)
                return -ENOMEM;

        ret = sysfs_create_file(kobj, &led_attr.attr);
        if(ret)
                kobject_put(kobj);

	return 0;
}
static int onboardled_remove(struct platform_device *pdev)
{
        kobject_put(kobj);
	pr_info("%s: Rmmod performed\n", __func__);
	return 0;
}

static const struct of_device_id onboardled_of_match_table[] = {
	{.compatible = "gpio-extled"},
	{},
};
MODULE_DEVICE_TABLE(of,onboardled_of_match_table);

static  struct platform_driver onboardled_driver={
	.driver = {
		.name          = "onboard_led",
		.owner         = THIS_MODULE,
		.of_match_table= onboardled_of_match_table,                     /* @of_match_table: The open firmware table.*/
	},
	.probe  =onboardled_probe,
	.remove =onboardled_remove,
		
};
static int __init led_init(void)
{
	pr_info("My first LED driver on BBB board\n");	
	return platform_driver_register(&onboardled_driver);
}

static void __exit led_exit(void)
{
	platform_driver_unregister(&onboardled_driver);
	pr_info("%s: My gpio node related driver successfully removed\n",__func__);	
}

module_init(led_init);
module_exit(led_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chandan Jha <beingchandanjha@gmail.com>");
MODULE_DESCRIPTION("platform driver for off-board led device");
MODULE_VERSION(".1");
