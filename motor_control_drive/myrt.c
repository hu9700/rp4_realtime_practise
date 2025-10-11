#define USE_GPIOD   (1)

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#if USE_GPIOD == 0
#include <linux/gpio.h>
#else
#include <linux/gpio/consumer.h>   // new GPIO descriptor API
#endif
#include <linux/interrupt.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/printk.h>

#define DEVICE_NAME "myrt"
#define CLASS_NAME  "myrtclass"

// GPIO config
#define GPIO_PWM    12   // output PWM
#define GPIO_MEAS   16   // input to measure rising edges

// PWM config (1 kHz)
#define PWM_PERIOD_NS 1000000L   // 1 ms = 1 kHz

#if USE_GPIOD
struct gpio_desc *gpio_pwm = NULL; 
struct gpio_desc *gpio_meas = NULL;
#endif

static int irq_number;
static ktime_t last_edge;
static u64 period_us = 0;
static struct hrtimer pwm_timer;
static ktime_t pwm_period;
static int duty_cycle = 50; // percent
static bool pwm_state = false;

// char device
static int    major;
static struct class*  myrt_class  = NULL;
static struct device* myrt_device = NULL;
static struct cdev my_cdev;

// ====== IRQ handler for GPIO16 rising edge ======
static irqreturn_t gpio_irq_handler(int irq, void *dev_id)
{
    ktime_t now = ktime_get();
    //if (!ktime_equal(last_edge, ktime_set(0,0))) {
    if (!ktime_compare(last_edge, ktime_set(0,0))) {
        s64 delta = ktime_us_delta(now, last_edge);
        period_us = delta;
    }
    last_edge = now;
    return IRQ_HANDLED;
}

// ====== hrtimer callback for PWM ======
static enum hrtimer_restart pwm_timer_callback(struct hrtimer *timer)
{
    static int counter = 0;
    ktime_t interval;

    counter = (counter + 1) % 100;
    if (counter < duty_cycle) {
#if USE_GPIOD == 0  
        gpio_set_value(GPIO_PWM, 1);
#else
        gpiod_set_value(gpio_pwm, 1);
#endif
    } else {
#if USE_GPIOD == 0
        gpio_set_value(GPIO_PWM, 0);
#else
        gpiod_set_value(gpio_pwm, 0);
#endif
    }

    interval = ktime_set(0, PWM_PERIOD_NS/100); // divide into 100 steps
    hrtimer_forward_now(timer, interval);
    return HRTIMER_RESTART;
}

// ====== File operations ======
static ssize_t myrt_read(struct file *filep, char __user *buffer,
                         size_t len, loff_t *offset)
{
    char msg[32];
    int msg_len = snprintf(msg, sizeof(msg), "%llu\n", period_us);
    if (*offset >= msg_len) return 0;
    if (copy_to_user(buffer, msg, msg_len)) return -EFAULT;
    *offset += msg_len;
    return msg_len;
}

static ssize_t myrt_write(struct file *filep, const char __user *buffer,
                          size_t len, loff_t *offset)
{
    char msg[16];
    if (len >= sizeof(msg)) return -EINVAL;
    if (copy_from_user(msg, buffer, len)) return -EFAULT;
    msg[len] = '\0';
    (void)kstrtoint(msg, 10, &duty_cycle);
    if (duty_cycle < 0) duty_cycle = 0;
    if (duty_cycle > 100) duty_cycle = 100;
    pr_info("myrt: duty cycle set to %d%%\n", duty_cycle);
    return len;
}

static struct file_operations fops = {
    .owner   = THIS_MODULE,
    .read    = myrt_read,
    .write   = myrt_write,
};

// ====== Init & Exit ======
static int __init myrt_init(void)
{
    int ret;

    // allocate char device
    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) {
        pr_err("failed to register char device\n");
        return major;
    }

    //myrt_class = class_create(THIS_MODULE, CLASS_NAME);
    myrt_class = class_create(CLASS_NAME);
    if (IS_ERR(myrt_class)) {
        if (PTR_ERR(myrt_class) == -EEXIST) {
            pr_warn("myrt: class '%s' already exists, removing and retrying\n", CLASS_NAME);
            // Remove old one manually
            class_destroy(myrt_class); // this will be NULL or invalid pointer
            myrt_class = NULL;

            // Try again after cleanup
            myrt_class = class_create(CLASS_NAME);
        }

        if (IS_ERR(myrt_class)) {
            unregister_chrdev(major, DEVICE_NAME);
            pr_err("myrt: failed to create class\n");
            return PTR_ERR(myrt_class);
        }
    }    

    myrt_device = device_create(myrt_class, NULL, MKDEV(major,0), NULL, DEVICE_NAME);
    if (IS_ERR(myrt_device)) {
        class_destroy(myrt_class);
        unregister_chrdev(major, DEVICE_NAME);
        pr_err("myrt: failed to create device\n");
        return PTR_ERR(myrt_device);
    }

    // setup GPIOs
#if USE_GPIOD == 0    
    if (!gpio_is_valid(GPIO_PWM) || !gpio_is_valid(GPIO_MEAS)) {
        pr_err("invalid GPIOs\n");
        return -ENODEV;
    }
    gpio_request(GPIO_PWM, "PWM_OUT");
    gpio_direction_output(GPIO_PWM, 0);

    gpio_request(GPIO_MEAS, "MEAS_IN");
    gpio_direction_input(GPIO_MEAS);
    irq_number = gpio_to_irq(GPIO_MEAS);
#else

    gpio_pwm  = gpiod_get(NULL, "PWM_OUT", GPIOD_OUT_LOW);
    if (IS_ERR(gpio_pwm)) return PTR_ERR(gpio_pwm);

    gpio_meas = gpiod_get(NULL, "MEAS_IN", GPIOD_IN);
    if (IS_ERR(gpio_meas)) return PTR_ERR(gpio_meas);

    irq_number = gpiod_to_irq(gpio_meas);
#endif

    ret = request_irq(irq_number, gpio_irq_handler,
                      IRQF_TRIGGER_RISING | IRQF_ONESHOT,
                      "myrt_gpio_irq", NULL);
    if (ret) {
        pr_err("Failed to request IRQ\n");
        return ret;
    }

    // setup PWM hrtimer
    pwm_period = ktime_set(0, PWM_PERIOD_NS/100); // 100 steps
    hrtimer_init(&pwm_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    pwm_timer.function = pwm_timer_callback;
    hrtimer_start(&pwm_timer, pwm_period, HRTIMER_MODE_REL);

    last_edge = ktime_set(0,0);
    pr_info("myrt: module loaded\n");
    return 0;
}

static void __exit myrt_exit(void)
{
    hrtimer_cancel(&pwm_timer);
    free_irq(irq_number, NULL);
#if USE_GPIOD == 0
    gpio_free(GPIO_PWM);
    gpio_free(GPIO_MEAS);
#else
    gpiod_put(gpio_pwm);
    gpiod_put(gpio_meas);
#endif
    device_destroy(myrt_class, MKDEV(major,0));
    class_destroy(myrt_class);
    unregister_chrdev(major, DEVICE_NAME);
    pr_info("myrt: module unloaded\n");
}

module_init(myrt_init);
module_exit(myrt_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hu");
MODULE_DESCRIPTION("RT PWM + GPIO interval driver");
