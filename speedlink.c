#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/can/netlink.h>
#include <net/if.h>
#include <getopt.h>

// Увеличим буфер до 8КБ, так как ответ ядра может быть объемным
#define NL_BUF_SIZE 8192

struct nl_req {
    struct nlmsghdr n;
    struct ifinfomsg i;
    char buf[NL_BUF_SIZE];
};

void add_attr(struct nlmsghdr *n, int type, const void *data, int alen) {
    int len = RTA_LENGTH(alen);
    struct rtattr *rta = (struct rtattr *)(((char *)n) + NLMSG_ALIGN(n->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len = len;
    if (data) memcpy(RTA_DATA(rta), data, alen);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + RTA_ALIGN(len);
}

int set_if_state(int sock, int ifindex, int up) {
    struct nl_req req;
    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.n.nlmsg_type = RTM_NEWLINK;
    req.i.ifi_index = ifindex;
    req.i.ifi_change |= IFF_UP;
    req.i.ifi_flags = up ? IFF_UP : 0;
    return send(sock, &req, req.n.nlmsg_len, 0);
}

uint32_t get_actual_bitrate(int sock, int ifindex) {
    struct {
        struct nlmsghdr n;
        struct ifinfomsg i;
        char buf[128]; // Буфер для расширенных атрибутов запроса
    } req;

    char res_buf[NL_BUF_SIZE];
    memset(&req, 0, sizeof(req));

    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_type = RTM_GETLINK;
    req.n.nlmsg_flags = NLM_F_REQUEST; 
    req.i.ifi_family = AF_UNSPEC;
    req.i.ifi_index = ifindex;

    // КРИТИЧЕСКИ ВАЖНО: Просим ядро прислать LINKINFO (включая параметры CAN)
    uint32_t ext_mask = 1; // RTEXT_FILTER_VF (иногда нужно для активации расширенного вывода)
    add_attr(&req.n, IFLA_EXT_MASK, &ext_mask, sizeof(ext_mask));

    if (send(sock, &req, req.n.nlmsg_len, 0) < 0) return 0;

    // Читаем ответ
    int len = recv(sock, res_buf, sizeof(res_buf), 0);
    if (len <= 0) return 0;

    struct nlmsghdr *nh = (struct nlmsghdr *)res_buf;
    
    // Пропускаем ACK от предыдущих команд, если они застряли в очереди
    while (NLMSG_OK(nh, len) && nh->nlmsg_type == NLMSG_ERROR) {
        struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nh);
        if (err->error != 0) return 0; // Реальная ошибка
        
        // Читаем следующее сообщение из этого же пакета или сокета
        len -= NLMSG_ALIGN(nh->nlmsg_len);
        nh = NLMSG_NEXT(nh, len);
        
        if (len <= 0) {
            len = recv(sock, res_buf, sizeof(res_buf), 0);
            nh = (struct nlmsghdr *)res_buf;
        }
    }

    if (!NLMSG_OK(nh, len) || nh->nlmsg_type != RTM_NEWLINK) return 0;

    struct rtattr *rta = IFLA_RTA(NLMSG_DATA(nh));
    int rta_len = IFLA_PAYLOAD(nh);

    for (; RTA_OK(rta, rta_len); rta = RTA_NEXT(rta, rta_len)) {
        if (rta->rta_type == IFLA_LINKINFO) {
            struct rtattr *li = RTA_DATA(rta);
            int li_len = RTA_PAYLOAD(rta);
            for (; RTA_OK(li, li_len); li = RTA_NEXT(li, li_len)) {
                if (li->rta_type == IFLA_INFO_DATA) {
                    struct rtattr *can = RTA_DATA(li);
                    int can_len = RTA_PAYLOAD(li);
                    for (; RTA_OK(can, can_len); can = RTA_NEXT(can, can_len)) {
                        if (can->rta_type == IFLA_CAN_BITTIMING) {
                            struct can_bittiming *bt = RTA_DATA(can);
                            return bt->bitrate;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    char *ifname = NULL;
    uint32_t target_bitrate = 0;
    int opt;

    while ((opt = getopt(argc, argv, "i:s:")) != -1) {
        if (opt == 'i') ifname = optarg;
        if (opt == 's') target_bitrate = (uint32_t)strtoul(optarg, NULL, 10);
    }

    if (!ifname || target_bitrate == 0) {
        printf("Использование: sudo %s -i can0 -s 1000000\n", argv[0]);
        return 1;
    }

    int ifindex = if_nametoindex(ifname);
    if (ifindex == 0) { perror("Интерфейс не найден"); return 1; }

    int sock = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (sock < 0) { perror("socket"); return 1; }

    set_if_state(sock, ifindex, 0); // Down
    usleep(50000);

    // Настройка (Bitrate)
    struct nl_req req;
    memset(&req, 0, sizeof(req));
    req.n.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifinfomsg));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.n.nlmsg_type = RTM_NEWLINK;
    req.i.ifi_index = ifindex;
    req.i.ifi_family = AF_UNSPEC;

    struct rtattr *linkinfo = (struct rtattr *)(((char *)&req) + NLMSG_ALIGN(req.n.nlmsg_len));
    add_attr(&req.n, IFLA_LINKINFO, NULL, 0);
    add_attr(&req.n, IFLA_INFO_KIND, "can", 3); // "can" без \0 или с ним

    struct rtattr *infodata = (struct rtattr *)(((char *)&req) + NLMSG_ALIGN(req.n.nlmsg_len));
    add_attr(&req.n, IFLA_INFO_DATA, NULL, 0);
    struct can_bittiming bt = { .bitrate = target_bitrate };
    add_attr(&req.n, IFLA_CAN_BITTIMING, &bt, sizeof(bt));

    infodata->rta_len = (char *)(((char *)&req) + NLMSG_ALIGN(req.n.nlmsg_len)) - (char *)infodata;
    linkinfo->rta_len = (char *)(((char *)&req) + NLMSG_ALIGN(req.n.nlmsg_len)) - (char *)linkinfo;

    // ... (код настройки битрейта) ...
    send(sock, &req, req.n.nlmsg_len, 0);
    usleep(50000);

    // Читаем битрейт ПОКА ИНТЕРФЕЙС ЕЩЕ DOWN
    uint32_t actual = get_actual_bitrate(sock, ifindex);

    // Теперь поднимаем
    set_if_state(sock, ifindex, 1);
    usleep(50000);

    if (actual == 0) {
        // Если все еще 0, попробуем прочитать после UP
        actual = get_actual_bitrate(sock, ifindex);
    }

    if (actual == target_bitrate) {
        printf("Успех! Битрейт установлен: %u\n", actual);
    } else {
        printf("Ошибка: задано %u, получено %u\n", target_bitrate, actual);
    }

    close(sock);
    return (actual == target_bitrate) ? 0 : 1;
}
