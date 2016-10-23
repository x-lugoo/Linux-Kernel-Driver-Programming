#include<linux/kernel.h>
#include<linux/module.h>
#include<linux/init.h>
#include<linux/hashtable.h>
#include<linux/fs.h>
#include<linux/types.h>
#include<linux/uaccess.h>
#include<linux/errno.h>
#include<linux/device.h>
#include<linux/slab.h>
#include<linux/cdev.h>
#include<linux/kdev_t.h>
#include<linux/miscdevice.h>
#include<linux/list.h>
#include<linux/gpio.h>
#include<linux/interrupt.h>
#include<linux/delay.h>
#include<linux/kthread.h>
#include<linux/ktime.h>
#include<linux/sched.h>
#include"hc_sr04_kernel.h"

static struct file_operations hc_sr04_fops = {
    .owner = THIS_MODULE,
    .open = hc_sr04_open,
    .read = hc_sr04_read,
    .write = hc_sr04_write,
    .unlocked_ioctl = hc_sr04_ioctl,
    .release = hc_sr04_release,
};

static struct miscdevice hc_sr04_A = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "HCSR_1",
    .fops = &hc_sr04_fops,
};

static struct miscdevice hc_sr04_B = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "HCSR_2",
    .fops = &hc_sr04_fops,
};

hcsr_struct hcA;
hcsr_struct hcB;


static LIST_HEAD(hcsr_list);


module_init(hc_sr04_init);
module_exit(hc_sr04_exit);


struct hcsr_struct* get_curr_hcsr(struct inode* node) {
    int minor;
    hcsr_struct *c;
    minor = iminor(node);
    list_for_each_entry(c, &hcsr_list, list) {
        if(c->hc_sr04->minor == minor) {
            return c;
        }
    }
    return NULL;
}

static int hc_sr04_open(struct inode* node, struct file* file) {
    struct hcsr_struct* curr_hcsr;
    printk(KERN_ALERT "hc_sr04: Open\n");
    curr_hcsr = get_curr_hcsr(node);
    file->private_data = (void*) curr_hcsr;
    printk(KERN_ALERT "Open: %s", curr_hcsr->hc_sr04->name);
    printk(KERN_ALERT "hc_sr04: Open Done\n");
    return 0;
}

static int hc_sr04_release(struct inode* node, struct file* file) {
    printk(KERN_ALERT "hc_sr04: Release\n");
    file->private_data = NULL;
    printk(KERN_ALERT "hc_sr04: Release Done\n");
    return 0;
}

static int do_send(struct hcsr_struct* hcsr){
    unsigned int counter = 0;
    char cnt = 0;
    int trig_pin = hcsr->kconfig.set.pins.trigger_pin;

    gpio_set_value(trig_pin, 1);
    udelay(10);
    gpio_set_value(trig_pin, 0);
    hcsr->kstart = ktime_get();
    
    spin_lock(&(hcsr->ongoing_lock));
    while(ONGOING == hcsr->ongoing) {
        if(0 == cnt){ 
            spin_unlock(&(hcsr->ongoing_lock));
            cnt++;
        }

        if(++counter > 50000) {
            spin_lock(&(hcsr->ongoing_lock));
            hcsr->ongoing = STOPPING;
            spin_unlock(&(hcsr->ongoing_lock));
            return -EAGAIN;
        }
        udelay(1);
    }

    if(0 == cnt) spin_unlock(&(hcsr->ongoing_lock));
    return 0;
}

static int send(struct hcsr_struct* hcsr, int retry_cnt) {
    int ret = 0;
    int send_cnt = 0;

    spin_lock(&(hcsr->ongoing_lock));
    if(ONGOING == hcsr->ongoing) {
        spin_unlock(&(hcsr->ongoing_lock));
        return 0;
    }
    else {
        hcsr->ongoing = ONGOING;
        spin_unlock(&(hcsr->ongoing_lock));
    }

    do {
        ret = do_send(hcsr);
    } while( (0 != ret)  && (send_cnt++ < retry_cnt));
    return ret;
}

static int thread_function(void* data) {
    struct hcsr_struct* hcsr = (struct hcsr_struct*) data;
    unsigned long ms = (1000)/(hcsr->kconfig.set.working_mode.freq);
    do {
        send(hcsr, 3);
        msleep_interruptible(ms);
    }
    while(!kthread_should_stop());


return 0;
}

static irqreturn_t echo_recv_isr(int irq, void *data) {
    //Only one isr_handler in the same time: IRQF_DISABLED, enable it at request_irq.
    ktime_t kend;
    unsigned int diff;
    unsigned int distance;
    struct hcsr_struct* hcsr = (struct hcsr_struct*) data;

    //printk(KERN_ALERT "echo_recv_isr\n");

    spin_lock(&(hcsr->ongoing_lock));
    if(STOPPING == hcsr->ongoing) {
        spin_unlock(&(hcsr->ongoing_lock));
        //printk(KERN_ALERT "!!! Signal from Dark Force !!!");
        goto RETURNED;
    } else {
        spin_unlock(&(hcsr->ongoing_lock));
    }


    kend = ktime_get();
    diff = ktime_to_ns(ktime_sub(kend, hcsr->kstart));
    distance = diff/5800;

    spin_lock(&(hcsr->cirb.cir_buf_lock));
        hcsr->cirb.newest = (hcsr->cirb.newest+1)%NUM_DATA;
        hcsr->cirb.buf.data[(hcsr->cirb.newest)] = distance;
        printk(KERN_ALERT "echo_recv_isr: %d CM, newest idx:%d, from %s, mode:%d \n",distance, hcsr->cirb.newest, hcsr->hc_sr04->name, hcsr->kconfig.set.working_mode.mode);
    spin_unlock(&(hcsr->cirb.cir_buf_lock));

RETURNED:
    spin_lock(&(hcsr->ongoing_lock));
    hcsr->ongoing = STOPPING;
    spin_unlock(&(hcsr->ongoing_lock));
    //printk(KERN_ALERT "echo_recv_isr Done\n");
    return IRQ_HANDLED;
}

static long ioctl_SETPINs(struct file* filp, unsigned long addr) {
    pin_set pins;
    int ret = 0;
    struct hcsr_struct* hcsr = NULL; 
    printk(KERN_ALERT "ioctl_SETPIN\n");
    if(0 != copy_from_user(&pins, (void*)addr, sizeof(pin_set))) {
        printk(KERN_ALERT "SETPIN: copy_from_user ERROR\n");
        ret = -EAGAIN;
        goto ERR_SETPIN_RETURN;
    }

    //hcsr = get_curr_hcsr(filp->f_dentry->d_inode);
    hcsr = (struct hcsr_struct*)filp->private_data;
    printk(KERN_ALERT "ioctl_SETPIN: Trig:%d, Echo:%d\n", pins.trigger_pin, pins.echo_pin);

    spin_lock(&(hcsr->irq_done_lock));
    if(IRQ_DONE == hcsr->irq_done) {
        free_irq(hcsr->echo_isr_number, hcsr);
    }
    freeTrig(hcsr->kconfig.set.pins.trigger_pin);
    freeEcho(hcsr->kconfig.set.pins.echo_pin);

    hcsr->kconfig.set.pins = pins;

    ret = setTrig(hcsr, hcsr->kconfig.set.pins.trigger_pin);
    if(ret) {
        printk(KERN_ALERT "setTrig Error,pin %d", hcsr->kconfig.set.pins.trigger_pin);
        goto ERR_SETPIN_RETURN;
    }
    ret = setEcho(hcsr, hcsr->kconfig.set.pins.echo_pin);
    if(ret) {
        printk(KERN_ALERT "setEcho Error,pin %d", hcsr->kconfig.set.pins.echo_pin);
        goto ERR_SETPIN_RETURN;
    }

    ret = set_ISR(hcsr);
    if(ret) {
        printk(KERN_ALERT "setEcho:  set_ISR failed \n");
        goto ERR_SETPIN_RETURN;
    }
    goto SUCCESS_SETPIN_RETURN;

ERR_SETPIN_RETURN:
    hcsr->kconfig.set.pins.trigger_pin = -1;
    hcsr->kconfig.set.pins.echo_pin = -1;
    spin_unlock(&(hcsr->irq_done_lock));
    return ret;

SUCCESS_SETPIN_RETURN:
    hcsr->irq_done = IRQ_DONE;
    spin_unlock(&(hcsr->irq_done_lock));
    printk(KERN_ALERT "ioctl_SETPIN Done: Trig:%d, Echo:%d\n", pins.trigger_pin, pins.echo_pin);
    return 0;
}


static long ioctl_SETMODE(struct file* file, unsigned long addr) {
    int ret = 0;
    mode_set working_mode;
    struct hcsr_struct* hcsr = NULL;

    printk(KERN_ALERT "ioctl_SETMODE\n");
    if(0 != copy_from_user(&working_mode, (void*)addr, sizeof(mode_set))) {
        printk(KERN_ALERT "SETPIN: copy_from_user ERROR\n");
        ret = -EAGAIN;
        goto ERR_SETMODE_RETURN;
    }
    if(ONE_SHOT != working_mode.mode && PERIODIC != working_mode.mode) return -EINVAL;
    

    hcsr = (struct hcsr_struct*) file->private_data;
    spin_lock(&(hcsr->kconfig.kconfig_lock));
    // still peeriodical sampling
    //before: one-shot ; after: one-shot
    /*For Periodic Task*/
    hcsr->kconfig.set.working_mode = working_mode;
    printk(KERN_ALERT "%s: mode:%d, freq:%d\n", hcsr->hc_sr04->name, hcsr->kconfig.set.working_mode.mode, hcsr->kconfig.set.working_mode.freq);
    spin_lock(&(hcsr->kthread_lock));
    if(ONE_SHOT != hcsr->kconfig.set.working_mode.mode && NULL != hcsr->kthread) {
        printk(KERN_ALERT "Keep Thread Going with period change: %s\n", hcsr->hc_sr04->name);
        kthread_stop(hcsr->kthread);
        hcsr->kthread = kthread_run (thread_function, hcsr, hcsr->hc_sr04->name);
    }else if(ONE_SHOT == hcsr->kconfig.set.working_mode.mode && NULL != hcsr->kthread) {
        printk(KERN_ALERT "Enter OneShot, Stop previous thread: %s\n", hcsr->hc_sr04->name);
        kthread_stop(hcsr->kthread);
        hcsr->kthread = NULL;
    }
    spin_unlock(&(hcsr->kthread_lock));
    spin_unlock(&(hcsr->kconfig.kconfig_lock));
    goto SUCCESS_SETMODE_RETURN;

ERR_SETMODE_RETURN:
        printk(KERN_ALERT "ioctl_SETMODE FAILED\n");
        return ret;

SUCCESS_SETMODE_RETURN:
    printk(KERN_ALERT "ioctl_SETMODE DONE\n");
    return 0;
}

static long hc_sr04_ioctl(struct file* file, unsigned int arg1, unsigned long arg2) {
    int ret;

    printk(KERN_ALERT "ioctl\n");
    switch (arg1) {
        case SETPINS:
            ret = ioctl_SETPINs(file, arg2);
            break;
        case SETMODE:
            ret = ioctl_SETMODE(file, arg2);
            break;
        default: 
            return -EINVAL;
    }

    printk(KERN_ALERT "ioctl DONE\n");
    return ret;
}
void spin_ongoing (struct hcsr_struct* hcsr) {
RETEST:
    if(ONGOING == hcsr->ongoing) {
        while(ONGOING == hcsr->ongoing) {
            udelay(1);
        }
        goto RETEST;
    }
    return;
}
static ssize_t hc_sr04_read(struct file *file, char *buf, size_t count, loff_t *ptr) {
    int ret = 0;
    struct hcsr_struct* hcsr = (struct hcsr_struct*) file->private_data;

    printk(KERN_ALERT "hc_sr04: Read\n");

    spin_lock(&(hcsr->kconfig.kconfig_lock));
    if(ONE_SHOT == hcsr->kconfig.set.working_mode.mode) {
        spin_unlock(&(hcsr->kconfig.kconfig_lock));

        do {
            spin_lock(&(hcsr->ongoing_lock));
            if(STOPPING == hcsr->ongoing) {
                spin_unlock(&(hcsr->ongoing_lock));
                send(hcsr, 5);
                break;
            } 
            spin_unlock(&(hcsr->ongoing_lock));
        }
        while(0);

            //Only One Shot,so Do best efforts to capture it.
            spin_ongoing(hcsr);

            spin_lock(&(hcsr->cirb.cir_buf_lock));
            ret = copy_to_user(buf, &(hcsr->cirb.buf.data[hcsr->cirb.newest]), sizeof(int));
            spin_unlock(&(hcsr->cirb.cir_buf_lock));
            if(ret) return -EAGAIN;
    } else {
        spin_unlock(&(hcsr->kconfig.kconfig_lock));

        spin_lock(&(hcsr->cirb.cir_buf_lock));
        if(-1 != hcsr->cirb.newest) {
            ret = copy_to_user(buf, &(hcsr->cirb.buf.data[hcsr->cirb.newest]), sizeof(int));
            spin_unlock(&(hcsr->cirb.cir_buf_lock));
        } else {
            spin_unlock(&(hcsr->cirb.cir_buf_lock));

RECHECK_BUF:
            spin_lock(&(hcsr->cirb.cir_buf_lock));
            printk("read Blocked:%s \n", hcsr->hc_sr04->name);
            if(-1 == hcsr->cirb.newest) {
                spin_unlock(&(hcsr->cirb.cir_buf_lock));
                msleep(10);
                goto RECHECK_BUF;
            }
            spin_unlock(&(hcsr->cirb.cir_buf_lock));

            spin_lock(&(hcsr->cirb.cir_buf_lock));
            printk(KERN_ALERT "read unBlocked-idx:%d,%d\n", hcsr->cirb.newest, (hcsr->cirb.buf.data[hcsr->cirb.newest]));
            ret = copy_to_user(buf, &(hcsr->cirb.buf.data[hcsr->cirb.newest]), sizeof(int));
            spin_unlock(&(hcsr->cirb.cir_buf_lock));
            if(ret) return -EAGAIN;
        }
    }

    printk(KERN_ALERT "hc_sr04: Read Done\n");
    return 0;
}


static ssize_t hc_sr04_write(struct file *file, const char __user *buf, size_t count, loff_t *ptr) {
    int act;
    int ret;
    //hcsr = get_curr_hcsr(file->f_dentry->d_inode);
    struct hcsr_struct* hcsr = (struct hcsr_struct*) file->private_data;

    printk(KERN_ALERT "hc_sr04: Write\n");

    ret = copy_from_user(&act , buf, sizeof(int));


    spin_lock(&(hcsr->kconfig.kconfig_lock));
    if(ONE_SHOT == hcsr->kconfig.set.working_mode.mode) {
        spin_unlock(&(hcsr->kconfig.kconfig_lock));

        if(0 != act) {
            spin_lock(&(hcsr->cirb.cir_buf_lock));
            memset(&(hcsr->cirb.buf), 0, sizeof(record));
            hcsr->cirb.newest = -1;
            spin_unlock(&(hcsr->cirb.cir_buf_lock));
        }

        spin_lock(&(hcsr->ongoing_lock));
        do {
            if(STOPPING == hcsr->ongoing) {
                spin_unlock(&(hcsr->ongoing_lock));
                send(hcsr, 5);
                break;
            } 
            spin_unlock(&(hcsr->ongoing_lock));
        } while(0);

    } else {
        spin_unlock(&(hcsr->kconfig.kconfig_lock));

        spin_lock(&(hcsr->kthread_lock));
        if(0 == act) {
            do {
                if(NULL == hcsr->kthread) {
                    break;
                }
                printk(KERN_ALERT "CALL ThreadStop");
                kthread_stop(hcsr->kthread);
                hcsr->kthread = NULL;

                spin_lock(&(hcsr->ongoing_lock));
                hcsr->ongoing = STOPPING;
                spin_unlock(&(hcsr->ongoing_lock));
            } while(0);
        } else {
            do {
                if(NULL != hcsr->kthread) {
                    break;
                }
                printk(KERN_ALERT "Write: Start thread from %s", hcsr->hc_sr04->name);
                hcsr->kthread = kthread_run(thread_function, hcsr, hcsr->hc_sr04->name);
            }while(0);
        }
        spin_unlock(&(hcsr->kthread_lock));

    } 
    printk(KERN_ALERT "hc_sr04: Write Done\n");
    return 0;
}


static int reg_misc(struct miscdevice* md) {
    int error;
    error = misc_register(md);
    if(error)
        printk(KERN_ALERT "Reg md Error:%s\n", md->name);

    return error;
}
static void dereg_misc(struct miscdevice* md) {
    misc_deregister(md);
}

static void init_hcsr_struct(hcsr_struct* hcsr, struct miscdevice* md, char(*pins)[5][2], char*(*pin_str)[5]) {
    /*!!!!!*/
    //CHECK!!! INIT
    hcsr->kthread = NULL;
    hcsr->hc_sr04 = md;
    hcsr->irq_done = IRQ_NOT_DONE;
    hcsr->echo_isr_number = -1;
    hcsr->pins_default = pins;
    hcsr->kconfig.set.working_mode.mode = 0;//__DEBUG__ ********
//    hcsr->kconfig.set.mode = -1; 
    hcsr->cirb.newest = -1;
    hcsr->pin_str = pin_str;
#ifdef __EXTENNTION__
    hcsr->pv.cnt = 0;
    sema_init(&(hcsr->pv.block_sem),0);
    spin_lock_init(&(hcsr->pv.cnt_lock));
    spin_lock_init(&(hcsr->irq_done_lock));
#endif
    spin_lock_init(&(hcsr->ongoing_lock));
    spin_lock_init(&(hcsr->cirb.cir_buf_lock));
    spin_lock_init(&(hcsr->kconfig.kconfig_lock));
    spin_lock_init(&(hcsr->kthread_lock));
    INIT_LIST_HEAD(&(hcsr->list));
    list_add(&(hcsr->list), &hcsr_list);
}


static int __init hc_sr04_init(void) {
    /*!!!!!*/
    //CHECK!!! INIT
    printk(KERN_ALERT "hc_sr04: INIT\n");
    if( reg_misc(&hc_sr04_A) || reg_misc(&hc_sr04_B) )
        return 0;

    init_hcsr_struct(&hcA, &hc_sr04_A, A_pins, A_pin_str);
    init_hcsr_struct(&hcB, &hc_sr04_B, B_pins, B_pin_str);

    printk(KERN_ALERT "hc_sr04: INIT DONE\n");
    return 0;
}

void free_gpio(struct hcsr_struct* hcsr) {
    freeTrig(hcsr->kconfig.set.pins.trigger_pin);
    freeEcho(hcsr->kconfig.set.pins.echo_pin);
}

static void __exit hc_sr04_exit(void) {
    printk(KERN_ALERT "hc_sr04: GoodBye Kernel World!!!\n");

    if(NULL != hcA.kthread)
        kthread_stop(hcA.kthread);
    if(IRQ_DONE == hcA.irq_done) {
        printk("---release hcA----\n");
        free_irq(hcA.echo_isr_number, &hcA);
        free_gpio(&hcA);  
    }

    if(NULL != hcB.kthread)
        kthread_stop(hcB.kthread);
    if(IRQ_DONE == hcB.irq_done) {
        printk("---release hcB----\n");
        free_irq(hcB.echo_isr_number, &hcB);
        free_gpio(&hcB);  
    }


    dereg_misc(&hc_sr04_A);
    dereg_misc(&hc_sr04_B);
    printk(KERN_ALERT "hc_sr04: EXIT DONE\n");
    return;
} 

MODULE_LICENSE("GPL");
