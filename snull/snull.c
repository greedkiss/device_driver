#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/interrupt.h>

#include <linux/in.h>
#include <linux/netdevice.h>   /* struct device, and other headers */
#include <linux/etherdevice.h> /* eth_type_trans */
#include <linux/ip.h>          /* struct iphdr */
#include <linux/tcp.h>         /* struct tcphdr */
#include <linux/skbuff.h>

#include <linux/in6.h>
#include <asm/checksum.h>

#include "snull.h"

MODULE_AUTHOR("HAHHAHA");
MODULE_LICENSE("Dual BSD/GPL");

static int lockup = 0;
module_param(lockup, int , 0);

static int timeout = SNULL_TIMEOUT;
module_param(timeout, int, 0);

static int use_napi = 0;
module_param(use_napi, int, 0);

struct snull_packet {
    struct snull_packet *next;
    struct net_device　*dev;
    int datalen;
    u8 data[ETH_DATA_LEN];
}


int pool_size = 8;
module_param(pool_size, int, 0);

struct snull_priv {
    struct net_device_stats stats;
    int status;
    struct snull_packet *ppool;
    struct snull_packet *rx_queue;
    int rx_int_enabled;
    int tx_packetlen;
    u8 *tx_packetdata;
    struct sk_buff * skb;
    spinlock_t lock;
    struct net_device *dev;
}

static void snull_tx_timeout(struct net_device *dev);
static void (*snull_interrupt)(int, void*, struct pt_regs *);

//建立pool
void snull_setup_pool(struct net_device* dev){
    struct snull_priv *priv = netdev_priv(dev);
    int i;
    struct snull_packet *pkt;

    priv->ppool = NULL;
    for(i = 0; i < pool_size; i++){
        pkt = kmalloc (sizeof(struct snull_packet), GFP_KERNEL);
        if(pkt == NULL){
            printk(KERN_NOTICE "MEMORY RUN OUT");
            return ;
        }

        //头插法
        pkt->dev = dev;
        pkt->next = priv->ppool;
        priv->ppool = pkt;
    }
}

//释放ppool
void snull_teardown_pool(struct net_device* dev){
    struct snull_packet * pkt;
    struct snull_priv * priv = netdev(dev);
    while(pkt = priv->ppool){
        kfree(priv->ppool);
        priv->ppool = pkt->next;
    }
}

//返回ppool首端的packet
struct snull_packet* snull_get_tx_buffer(struct net_device * dev){
    struct snull_priv *priv = netdev_priv(dev);
    struct snull_packet* pkt;
    unsigned long flags;

    spin_lock_irqsave(&priv->lock, flags);
    pkt = priv->ppool;
    priv->ppool = pkt->next;
    if(priv->ppool == NULL){
        printk(KERN_INFO "Pool empty");
        netif_stop_queue(dev);
    }
    spin_unlock_irqrestore(&priv->lock, flags);
    return pkt;
}

//往ppool加数据
void snull_release_buffer(struct snull_packet* pkt){
    unsigned long flags;
    struct snull_priv * priv = netdev_priv(pkt->dev);

    //禁止终端,信息保存在flags中
    spin_lock_irqsave(&priv->lock, flags);
    pkt->next = priv->ppool;
    priv->ppool = pkt;
    spin_unlock_irqrestore(&priv->lock, flags);
    if(netif_queue_stopped(pkt->dev) && pkt->next == NULL)
        netif_wake_queue(pkt->dev);
}

//往rx_queue加数据
void snull_enqueue_buf(struct net_device * dev, struct snull_packet *pkt){
    unsigned long flags;
    struct snull_packet *priv = netdev_priv(dev);
    
    spin_lock_irqsave(&priv->lock, flags);
    pkt->next = priv->rx_queue;
    priv->rx_queue = pkt;
    spin_unlock_irqrestore(&priv->lock, flags);
}

//拿除rx_queue的一个数据
struct snull_packet* snull_dequeue_buf(struct net_device * dev){
    struct snull_priv* priv = netdev_priv(dev);
    struct snull_packet* pkt;
    unsigned long flags;

    spin_lock_irqsave(&priv->lock, flags);
    pkt = priv->rx_queue;
    if(!pkt->next)
        priv->rx_queue = pkt->next;
    spin_unlock_irqrestore(&priv->lock, flags);
    return pkt;
}

//开启接收中断
static void snull_rx_ints(struct net_device* dev, int enable){
    struct snull_priv * priv = netdev_priv(dev);
    priv->rx_int_enabled = enable;
}

//复制硬件mac地址,开启queue
int snull_open(struct net_device * dev){
    memcpy(dev->dev_addr, "\0SNUL0", ETH_ALEN);
	if (dev == snull_devs[1])
		dev->dev_addr[ETH_ALEN-1]++; /* \0SNUL1 */
	netif_start_queue(dev);
	return 0;
}

int snull_release(struct net_device * dev){
    netif_stop_queue(dev);
    return 0;
}

int snull_config(struct net_device *dev, struct ifmap* map){
    if (dev->flags & IFF_UP) /* can't act on a running interface */
		return -EBUSY;

	/* Don't allow changing the I/O address */
	if (map->base_addr != dev->base_addr) {
		printk(KERN_WARNING "snull: Can't change I/O address\n");
		return -EOPNOTSUPP;
	}

	/* Allow changing the IRQ */
	if (map->irq != dev->irq) {
		dev->irq = map->irq;
        	/* request_irq() is delayed to open-time */
	}

	/* ignore other fields */
	return 0;
}

void snull_rx(struct net_device* dev, struct snull_packet * pkt){
    struct sk_buff * skb;
    struct snull_priv * priv = netdev_priv(dev);

    skb = dev_alloc_skb(pkt->datalen+2);
    if(!skb){
        if (printk_ratelimit())
			printk(KERN_NOTICE "snull rx: low on mem - packet dropped\n");
		priv->stats.rx_dropped++;
		goto out
    }
    skb_reverse(skb,2);
    memcpy(skb_put(skb, pkt->datalen), pkt->data, pkt->datalen);

    skb->dev = dev;
    skb->protocol = eth_type_trans(skb, dev);
    skb->ip_summed = CHECKSUM_UNNECESSARY;
    priv->stats.rx_packets++;
    priv->stats.rx_bytes+=pkt->datalen;
    netif_rx(skb);
out:
    return ;
}



module_init(snull_init_module);
module_exit(snull_cleanup);