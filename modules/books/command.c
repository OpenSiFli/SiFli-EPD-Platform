#include "bookshelf.h"
#include "reader.h"
#include <stdlib.h>
#include <string.h>

static void cmd_bs_list(int argc, char **argv)
{
    bookshelf_item_t item;
    int i;
    int n;

    (void)argc;
    (void)argv;

    n = bookshelf_refresh();
    rt_kprintf("bookshelf \"%s\": %d book(s)\n", bookshelf_dir(), n);

    for (i = 0; i < n; i++)
    {
        if (bookshelf_get(i, &item))
        {
            rt_kprintf("  [%d] %-32s %u bytes  progress %d%%\n",
                       i, item.name, (unsigned)item.size, item.progress);
        }
    }
}

/*---------------------------------------------------------------------------*/
/* reader 子命令 */
/*---------------------------------------------------------------------------*/
static int reader_get_len_arg(int argc, char **argv, int arg_index)
{
    int len = 512;

    if (argc > arg_index)
    {
        len = atoi(argv[arg_index]);
        if (len < 16)
            len = 16;
        if (len > 2048)
            len = 2048;
    }

    return len;
}

static void cmd_rd_open(int argc, char **argv)
{
    rt_err_t ret;

    if (argc < 2)
    {
        rt_kprintf("usage: svc reader open <path>\n");
        return;
    }

    ret = reader_open(argv[1]);
    rt_kprintf("open \"%s\": %s\n", argv[1], (ret == RT_EOK) ? "ok" : "FAILED");
}

static void cmd_rd_info(int argc, char **argv)
{
    reader_info_t info;

    (void)argc;
    (void)argv;

    if (!reader_get_info(&info))
    {
        rt_kprintf("no book opened (svc reader open <path>)\n");
        return;
    }

    rt_kprintf("title    : %s\n", info.title);
    rt_kprintf("path     : %s\n", info.path);
    rt_kprintf("size     : %u bytes\n", (unsigned)info.file_size);
    rt_kprintf("encoding : %s\n", info.encoding);
    rt_kprintf("position : %u (%d%%)\n", (unsigned)info.position, reader_get_percent());
}

static void cmd_rd_read(int argc, char **argv)
{
    uint32_t offset;
    uint32_t next = 0;
    int len;
    char *buf;
    int n;

    if (!reader_is_open())
    {
        rt_kprintf("no book opened (svc reader open <path>)\n");
        return;
    }

    offset = (argc > 1) ? (uint32_t)strtoul(argv[1], RT_NULL, 0) : reader_get_position();
    len = reader_get_len_arg(argc, argv, 2);

    buf = rt_malloc((rt_size_t)len + 1);
    if (buf == RT_NULL)
    {
        rt_kprintf("no memory\n");
        return;
    }

    n = reader_read_text(offset, buf, (rt_size_t)len + 1, &next);
    if (n < 0)
    {
        rt_kprintf("read failed (%d)\n", n);
    }
    else
    {
        rt_kprintf("---- read @%u, %d bytes, next %u ----\n",
                   (unsigned)offset, n, (unsigned)next);
        rt_kprintf("%s\n", buf);
        rt_kprintf("---- end ----\n");
    }

    rt_free(buf);
}

static void cmd_rd_next(int argc, char **argv)
{
    int len;
    char *buf;
    int n;

    if (!reader_is_open())
    {
        rt_kprintf("no book opened (svc reader open <path>)\n");
        return;
    }

    len = reader_get_len_arg(argc, argv, 1);

    buf = rt_malloc((rt_size_t)len + 1);
    if (buf == RT_NULL)
    {
        rt_kprintf("no memory\n");
        return;
    }

    n = reader_read_next(buf, (rt_size_t)len + 1);
    if (n < 0)
    {
        rt_kprintf("read failed (%d)\n", n);
    }
    else
    {
        rt_kprintf("---- read %d bytes, position now %u (%d%%) ----\n",
                   n, (unsigned)reader_get_position(), reader_get_percent());
        rt_kprintf("%s\n", buf);
        rt_kprintf("---- end ----\n");
    }

    rt_free(buf);
}

static void cmd_rd_seek(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("usage: svc reader seek <offset>\n");
        return;
    }

    reader_set_position((uint32_t)strtoul(argv[1], RT_NULL, 0));
    rt_kprintf("position: %u (%d%%)\n", (unsigned)reader_get_position(), reader_get_percent());
}

static void cmd_rd_close(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    reader_close();
    rt_kprintf("closed\n");
}


typedef struct
{
    const char *name, *usage;
    void (*handler)(int argc, char **argv);
} svc_cmd_t;
static const svc_cmd_t bookshelf_cmds[] =
{
    { "list",    "scan and list books in TF card root", cmd_bs_list },
};

static const svc_cmd_t reader_cmds[] =
{
    { "open",    "open book: open <path>",           cmd_rd_open },
    { "info",    "current book info",                cmd_rd_info },
    { "read",    "read: read [offset] [len]",        cmd_rd_read },
    { "next",    "read from position: next [len]",   cmd_rd_next },
    { "seek",    "set position: seek <offset>",      cmd_rd_seek },
    { "close",   "close book",                       cmd_rd_close },
};


int books_command(int argc, char **argv)
{
    const svc_cmd_t *commands;
    unsigned count;
    if (argc && !strcmp(argv[0], "bookshelf"))
    {
        commands = bookshelf_cmds;
        count = sizeof(bookshelf_cmds) / sizeof(bookshelf_cmds[0]);
    }
    else if (argc && !strcmp(argv[0], "reader"))
    {
        commands = reader_cmds;
        count = sizeof(reader_cmds) / sizeof(reader_cmds[0]);
    }
    else
    {
        rt_kprintf("svc books bookshelf|reader <command> [args]\n");
        return RT_EOK;
    }
    if (argc < 2)
    {
        for (unsigned i = 0; i < count; ++i)
            rt_kprintf("  %-10s %s\n", commands[i].name, commands[i].usage);
        return RT_EOK;
    }
    for (unsigned i = 0; i < count; ++i)
        if (!strcmp(commands[i].name, argv[1]))
        {
            commands[i].handler(argc - 1, argv + 1);
            return RT_EOK;
        }
    return -RT_ENOSYS;
}
