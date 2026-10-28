/*
 * SPDX-FileCopyrightText: 2024-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/**
 * @file service_cmd.c
 * @brief 服务管理命令（msh）：查询服务状态并调用服务接口。
 *
 * 用法：
 *   svc                                  列出内置服务命令
 *   svc pan status|on|off|connect|ip       蓝牙 PAN 状态与控制
 *   svc <app> [args]                      转发动态应用命令
 */
#include "app_service_internal.h"
#include <rtthread.h>
#include <string.h>

#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"

#include "app_service.h"
#include "bt_pan.h"

/*---------------------------------------------------------------------------*/
/* 小工具 */
/*---------------------------------------------------------------------------*/
static const char *pan_state_name(btpan_state_t st)
{
    switch (st)
    {
    case BTPAN_STATE_OFF:           return "OFF";
    case BTPAN_STATE_INITIALIZING:  return "INITIALIZING";
    case BTPAN_STATE_READY:         return "READY (waiting phone)";
    case BTPAN_STATE_CONNECTED:     return "CONNECTED (network pending)";
    case BTPAN_STATE_NETWORK_READY: return "NETWORK_READY (online)";
    default:                        return "?";
    }
}

/*---------------------------------------------------------------------------*/
/* pan 子命令 */
/*---------------------------------------------------------------------------*/
static void cmd_pan_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    rt_kprintf("state          : %s\n", pan_state_name(btpan_get_state()));
    rt_kprintf("stack ready    : %s\n", btpan_is_ready() ? "yes" : "no");
    rt_kprintf("phone connected: %s\n", btpan_is_connected() ? "yes" : "no");
    rt_kprintf("network ready  : %s\n", btpan_is_network_ready() ? "yes" : "no");
}

static void cmd_pan_on(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("btpan_enable(true): %d\n", btpan_enable(true));
}

static void cmd_pan_off(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("btpan_enable(false): %d\n", btpan_enable(false));
}

static void cmd_pan_connect(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    rt_kprintf("btpan_request_connect: %d\n", btpan_request_connect());
}

static void cmd_pan_ip(int argc, char **argv)
{
    struct netif *netif;
    char buf[IP4ADDR_STRLEN_MAX];
    char buf0[IP4ADDR_STRLEN_MAX];
    char buf1[IP4ADDR_STRLEN_MAX];

    (void)argc;
    (void)argv;

    /* bt_lwip 注册的 PAN 网卡名为 "b0" */
    netif = netif_find("b0");
    if (netif == RT_NULL)
    {
        rt_kprintf("PAN netif (b0) not found - bluetooth PAN not initialized\n");
        return;
    }

    rt_kprintf("netif     : b0\n");
    rt_kprintf("link      : %s\n", netif_is_link_up(netif) ? "up" : "down");
    rt_kprintf("dhcp      : %s\n", dhcp_supplied_address(netif) ? "leased" : "no lease");
    rt_kprintf("IP        : %s\n", ip4addr_ntoa_r(netif_ip4_addr(netif), buf, sizeof(buf)));
    rt_kprintf("Netmask   : %s\n", ip4addr_ntoa_r(netif_ip4_netmask(netif), buf, sizeof(buf)));
    rt_kprintf("Gateway   : %s\n", ip4addr_ntoa_r(netif_ip4_gw(netif), buf, sizeof(buf)));
    rt_kprintf("DNS       : %s / %s\n",
               ipaddr_ntoa_r(dns_getserver(0), buf0, sizeof(buf0)),
               ipaddr_ntoa_r(dns_getserver(1), buf1, sizeof(buf1)));
}

/*---------------------------------------------------------------------------*/
/* 命令表 */
/*---------------------------------------------------------------------------*/
typedef struct
{
    const char *name;
    const char *usage;
    void (*handler)(int argc, char **argv);
} svc_cmd_t;

typedef struct
{
    const char *name;
    const svc_cmd_t *cmds;
    rt_size_t cmd_num;
} svc_group_t;

static const svc_cmd_t pan_cmds[] =
{
    { "status",  "print pan state",                  cmd_pan_status },
    { "on",      "enable bluetooth pan service",     cmd_pan_on },
    { "off",     "disable bluetooth pan service",    cmd_pan_off },
    { "connect", "request pan connection",           cmd_pan_connect },
    { "ip",      "show PAN netif IP address (DHCP)", cmd_pan_ip },
};

static const svc_group_t g_groups[] =
{
    { "pan",       pan_cmds,       sizeof(pan_cmds) / sizeof(pan_cmds[0]) },
};

#define SVC_GROUP_COUNT (sizeof(g_groups) / sizeof(g_groups[0]))

/**
 * @brief msh 入口：svc <service> <sub-command> [args]
 */
static void svc(int argc, char **argv)
{
    const svc_group_t *grp = RT_NULL;
    const svc_cmd_t *cmd = RT_NULL;
    rt_size_t i;

    if (argc < 2)
    {
        rt_kprintf("data service commands:\n");
        for (i = 0; i < SVC_GROUP_COUNT; i++)
            rt_kprintf("  svc %-10s (%u sub-commands)\n",
                       g_groups[i].name, (unsigned)g_groups[i].cmd_num);
        rt_kprintf("use \"svc <service>\" to list sub-commands\n");
        return;
    }

    for (i = 0; i < SVC_GROUP_COUNT; i++)
    {
        if (rt_strcmp(g_groups[i].name, argv[1]) == 0)
        {
            grp = &g_groups[i];
            break;
        }
    }

    if (grp == RT_NULL)
    {
        if (!strcmp(argv[1], "bookshelf") || !strcmp(argv[1], "reader"))
        {
            if (app_service_command("books", argc - 1, argv + 1) == RT_EOK) return;
        }
        if (app_service_command(argv[1], argc - 2, argv + 2) == RT_EOK) return;
        rt_kprintf("svc: unknown service \"%s\" (try \"svc\")\n", argv[1]);
        return;
    }

    if (argc < 3)
    {
        rt_kprintf("usage: svc %s <sub-command> [args]\n", grp->name);
        for (i = 0; i < grp->cmd_num; i++)
            rt_kprintf("  %-10s %s\n", grp->cmds[i].name, grp->cmds[i].usage);
        return;
    }

    for (i = 0; i < grp->cmd_num; i++)
    {
        if (rt_strcmp(grp->cmds[i].name, argv[2]) == 0)
        {
            cmd = &grp->cmds[i];
            break;
        }
    }

    if (cmd == RT_NULL)
    {
        rt_kprintf("svc %s: unknown sub-command \"%s\"\n", grp->name, argv[2]);
        return;
    }

    cmd->handler(argc - 2, argv + 2);
}
MSH_CMD_EXPORT(svc, data service management: svc <service> <cmd> [args]);
