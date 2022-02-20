#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/inet.h>
#include <net/ip.h>
#include <net/route.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/byteorder/generic.h>
#include <linux/timer.h>
#include <linux/types.h>
#include <linux/jiffies.h>


// LICENSE
MODULE_LICENSE("GPL");

// 作者
MODULE_AUTHOR("Dong_Yinqing_202121220330");

// 描述
MODULE_DESCRIPTION("vni module");

// 模块版本
MODULE_VERSION("0.2");

#define ETH_P_VNI 0xf4f0     //VNI协议类型号
#define VNI_LLEN 4          // 学号长度



struct vnihdr
{
    unsigned char label[VNI_LLEN];// 学号
    unsigned short seq;           // 序号
};
const char *eth_name = "ens33";          //本机网卡名
const char  *host_ip =  "10.168.1.207"; //本机IP地址
const unsigned char H_SOURCE[] = {0x00,0x0c,0x29,0xdb,0x39,0x41}; // 网卡MAC地址
const unsigned char BROAD_DEST[] = {0xff,0xff,0xff,0xff,0xff,0xff};
const unsigned char MY_SEQ[] = {0X00,0X03,0X03,0X00}; //学号
unsigned int pkt_headlen = 20; // 封装IP包的帧的头长度
const int T = 60;              // 定时器周期=T/D，单位s
const int D = 1;               
static struct nf_hook_ops nfho;
struct timer_list mytimer;
// unsigned int tx_packets;
// unsigned int rx_packets;
atomic_t tx_packets = ATOMIC_INIT(0);
atomic_t rx_packets = ATOMIC_INIT(0);
static int tx_count;
static int rx_count;
static int tx_temp;
static int rx_temp;
static int tx_rate;
static int rx_rate;



void vni_state_info(struct timer_list *arg) // 如果不修改超时值，则超时之后只会执行一次
{
    printk("%s\n", __func__);
    tx_temp = atomic_read(&tx_packets);
    rx_temp = atomic_read(&rx_packets);
    tx_rate = 10*D*(tx_temp-tx_count)/T;
    rx_rate = 10*D*(rx_temp-rx_count)/T;
    printk("the number of vni tx_packet:%u\n", tx_temp);
    printk("the number of vni rx_packet:%u\n", rx_temp);
    printk("Packet sending rate:%d.%dpps\n", tx_rate/10, tx_rate%10);
    printk("Packet reception rate:%d.%dpps\n", rx_rate/10, rx_rate%10);
    tx_count = tx_temp;
    rx_count = rx_temp;
    // mytimer.expires = jiffies + T * HZ / D;  
    if (time_before(jiffies, mytimer.expires + T * HZ / D)) //#define time_before(unkonwn,known) ((long)(unknown) - (long)(known)<0)
    {
        mod_timer(&mytimer, mytimer.expires + T * HZ / D);  // 修改定时器的expire
    }
}

static int vni_send_state(struct sk_buff *skb)
{
    // 统计已发送的数据包
    atomic_inc_return(&tx_packets);
    return 0;
}

static int vni_recv_state(struct sk_buff *skb)
{
    // 统计已接收的数据包
    atomic_inc_return(&rx_packets);
    return 0;
}


// 发送模块
unsigned int vni_skb_send(void *priv, struct sk_buff *skb, 
    const struct nf_hook_state *state)
{  
    int nret;
    struct sk_buff *skb_cp;
    struct ethhdr *mac_header; 
    struct vnihdr *vni_header;
    unsigned short seq;
    int seq_temp;

    if (strcmp(skb->dev->name,eth_name) != 0)
    {
        return NF_ACCEPT;
    }
    //发送
    nret = 1;
    if(skb_headroom(skb) < pkt_headlen+2)
    {
        skb_cp = skb_copy_expand(skb,pkt_headlen+2,0,GFP_ATOMIC);            
	}
    else
    {
    	skb_cp = skb_copy(skb, GFP_ATOMIC);
    }
    // skb_cp = skb_copy_expand(skb, pkt_headlen+2 ,0, GFP_ATOMIC);
    seq_temp = atomic_read(&tx_packets);
    seq = seq_temp% 65536; // seq 为 unsigned short 取值范围0-65535 初始序号从0开始
    vni_header = (struct vnihdr*)skb_push(skb_cp, sizeof(struct vnihdr));
    memcpy(vni_header->label, MY_SEQ, VNI_LLEN);
    vni_header->seq = __constant_htons(seq);

    mac_header = (struct ethhdr*)skb_push(skb_cp, sizeof(struct ethhdr));
    memcpy(mac_header->h_dest, BROAD_DEST, ETH_ALEN);
    memcpy(mac_header->h_source, H_SOURCE, ETH_ALEN);
    mac_header->h_proto = __constant_htons(ETH_P_VNI);

    if(dev_queue_xmit(skb_cp) < 0)
    {
        printk("dev_queue_xmit error\n");
        goto out;
    }
    vni_send_state(skb); //统计

    nret = 0;//这里是必须的
out:
    if(0 != nret && NULL != skb_cp)
    {
        dev_put(skb_cp->dev);//减少设备的引用计数
        kfree_skb(skb_cp);//销毁数据包/
    }

    return NF_DROP;
}


// 接收模块
int vni_skb_recv(struct sk_buff *skb, struct net_device *dev,
 struct packet_type *ptype, struct net_device *orig_dev)
{
    struct iphdr *ip_header;
    struct vnihdr *vni_header;
    unsigned int ip_dst;

    vni_header = (struct vnihdr *)skb->data;
    ip_header = (struct iphdr *)(skb->data+sizeof(struct vnihdr));
    ip_dst = ip_header->daddr;  
    if (ip_dst != in_aton(host_ip))
    {
        kfree_skb(skb);
        return NET_RX_DROP;
    }
    skb_pull(skb, sizeof(struct vnihdr)); // 移动data指针去掉VNI头
    vni_recv_state(skb);
    skb->pkt_type = PACKET_HOST;
    skb->protocol = htons(ETH_P_IP);
    netif_rx(skb);
    return NET_RX_SUCCESS;
}

static struct packet_type vni_packet_type __read_mostly = {
.type = cpu_to_be16(ETH_P_VNI),
.func = vni_skb_recv, 
};

int init_module()
{
    pr_info("----------vni module v0.2-------------------\n");
    pr_info("----author:dong_yinqing_202121220330-------\n");
    tx_count = 0;
    rx_count = 0;

    nfho.hook = vni_skb_send;
    nfho.hooknum = NF_INET_POST_ROUTING;
    nfho.pf = PF_INET;
    nfho.priority = NF_IP_PRI_FIRST;
    nf_register_net_hook(&init_net, &nfho);
    
    dev_add_pack(&vni_packet_type);

    timer_setup(&mytimer, vni_state_info, 0);
    mytimer.expires = jiffies + T * HZ / D;
    add_timer(&mytimer);              // 激活动态定时器

    return 0;
}

void cleanup_module()
{
    pr_info("----------vni module rmmod-------------------\n");
    // Unregister the hook function
    nf_unregister_net_hook(&init_net, &nfho);
    dev_remove_pack(&vni_packet_type);
    del_timer(&mytimer);              // 删除定时器变量
}
