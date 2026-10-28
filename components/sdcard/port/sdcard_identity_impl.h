/* SPDX-License-Identifier: LicenseRef-SiFli */
/* Included by the SiFli port. The SDK registers host->name as the whole-card
 * device in rt_mmcsd_blk_probe(); its partition-device branch is disabled.
 * This adapter supports that mapping with RT_MMCSD_USER_OFFSET == 0 only.
 * FAT partition offsets come from the on-media MBR, never device private data. */
static struct
{
    struct rt_mmcsd_host *host;
    struct rt_mmcsd_card *card;
    rt_bool_t (*current)(void *);
    void *context;
    uint32_t cid[4];
    rt_bool_t failed;
} s_guard;

static rt_bool_t _guard_current(void)
{
    if (!s_guard.host || s_guard.failed || !s_guard.current(s_guard.context) ||
        s_guard.host->card != s_guard.card ||
        memcmp(s_guard.card->resp_cid, s_guard.cid, sizeof(s_guard.cid)))
    {
        s_guard.failed = RT_TRUE;
        return RT_FALSE;
    }
    return RT_TRUE;
}

rt_err_t sdcard_port_guard_begin(struct rt_mmcsd_host *host,
                                rt_bool_t (*current)(void *), void *context)
{
    sdcard_port_status_t status;
    if (!host || !current || s_guard.host) return -RT_EBUSY;
    sdcard_port_get_status(host, &status);
    if (status.change == SDCARD_CHANGE_PENDING || !status.sd_memory ||
        !status.block_device || !current(context)) return -RT_EBUSY;
    mmcsd_host_lock(host);
    if (!host->card)
    {
        mmcsd_host_unlock(host);
        return -RT_ENOSYS;
    }
    memset(&s_guard, 0, sizeof(s_guard));
    s_guard.host = host;
    s_guard.card = host->card;
    s_guard.current = current;
    s_guard.context = context;
    memcpy(s_guard.cid, host->card->resp_cid, sizeof(s_guard.cid));
    mmcsd_host_unlock(host);
    return RT_EOK;
}

void sdcard_port_guard_end(struct rt_mmcsd_host *host)
{
    RT_ASSERT(host == s_guard.host);
    memset(&s_guard, 0, sizeof(s_guard));
}

static uint32_t _le16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t _le32(const uint8_t *p)
{
    return _le16(p) | (_le16(p + 2) << 16);
}

static rt_err_t _identity_read(rt_device_t device, uint32_t sector, uint8_t *buffer)
{
    rt_size_t read;
    if (!_guard_current()) return -RT_EBUSY;
    read = rt_device_read(device, (rt_off_t)sector, buffer, 1);
    if (!_guard_current()) return -RT_EBUSY;
    if (read != 1) return -RT_ERROR;
    return RT_EOK;
}

/* Validate enough of the BPB to identify a FAT volume without mounting it or
 * trusting its free-space tables. FAT allocation/directory damage is allowed. */
static rt_err_t _fat_identity(sdcard_identity_t *out, uint32_t limit)
{
    const uint8_t *b = out->volume_boot;
    uint32_t total, fat, reserved, roots, clusters, bits, serial_offset;
    uint64_t overhead, fat_entries;
    if (!memcmp(b + 3, "EXFAT   ", 8)) return -RT_ENOSYS;
    if (b[510] != 0x55 || b[511] != 0xaa ||
        !(b[0] == 0xe9 || (b[0] == 0xeb && b[2] == 0x90)) ||
        _le16(b + 11) != 512 || !b[13] || (b[13] & (b[13] - 1)) ||
        b[13] > 128 || !b[16] || b[16] > 2) return -RT_ERROR;
    reserved = _le16(b + 14);
    roots = _le16(b + 17);
    total = _le16(b + 19);
    if (!total) total = _le32(b + 32);
    fat = _le16(b + 22);
    if (!fat) fat = _le32(b + 36);
    overhead = reserved + (uint64_t)b[16] * fat + ((roots * 32 + 511) / 512);
    if (!reserved || !fat || !total || total > limit || overhead >= total)
        return -RT_ERROR;
    clusters = (uint32_t)((total - overhead) / b[13]);
    bits = clusters < 4085 ? 12u : (clusters < 65525 ? 16u : 32u);
    if (!clusters || clusters >= 0x0ffffff5u) return -RT_ERROR;
    if (bits == 32)
    {
        if (roots || _le16(b + 22) || _le16(b + 42) ||
            _le32(b + 44) < 2 || _le32(b + 44) >= clusters + 2)
            return -RT_ERROR;
        serial_offset = 67;
    }
    else
    {
        if (!roots || !_le16(b + 22)) return -RT_ERROR;
        serial_offset = 39;
    }
    fat_entries = (uint64_t)fat * 512 * 8 / bits;
    if (fat_entries < (uint64_t)clusters + 2 || b[serial_offset - 1] != 0x29)
        return -RT_ERROR;
    out->volume_serial = _le32(b + serial_offset);
    out->volume_sector_count = total;
    out->fat_bits = bits;
    return RT_EOK;
}

rt_err_t sdcard_port_identity(struct rt_mmcsd_host *host, const char *device_name,
                             sdcard_identity_t *out)
{
    rt_device_t device;
    struct rt_device_blk_geometry geometry;
    uint8_t first[512];
    uint32_t start, count;
    unsigned i;
    rt_bool_t opened = RT_FALSE;
    rt_err_t error;
    if (!out) return -RT_EINVAL;
    memset(out, 0, sizeof(*out));
    if (host != s_guard.host || !_guard_current()) return -RT_EBUSY;
#if defined(RT_MMCSD_USER_OFFSET) && RT_MMCSD_USER_OFFSET != 0
    return -RT_ENOSYS;
#endif
    if (strcmp(device_name, host->name)) return -RT_ENOSYS;
    device = rt_device_find(device_name);
    if (!device || device->type != RT_Device_Class_Block) return -RT_ERROR;
    if (!device->ref_count)
    {
        error = rt_device_open(device, RT_DEVICE_OFLAG_RDWR);
        if (error != RT_EOK) return error;
        opened = RT_TRUE;
    }
    memset(&geometry, 0, sizeof(geometry));
    error = rt_device_control(device, RT_DEVICE_CTRL_BLK_GETGEOME, &geometry);
    if (error != RT_EOK) goto done;
    if (!geometry.sector_count || geometry.bytes_per_sector != 512 ||
        (uint64_t)geometry.sector_count != (uint64_t)s_guard.card->card_capacity * 2)
    {
        error = -RT_ENOSYS;
        goto done;
    }
    error = _identity_read(device, 0, first);
    if (error != RT_EOK) goto done;
    out->sector_start = 0;
    out->sector_count = geometry.sector_count;
    out->sector_size = 512;
    out->partition_sector_count = geometry.sector_count;
    memcpy(out->cid, s_guard.cid, sizeof(out->cid));
    if (!(out->cid[0] | out->cid[1] | out->cid[2] | out->cid[3]))
    {
        error = -RT_ERROR;
        goto done;
    }
    memcpy(out->volume_boot, first, sizeof(first));
    error = _fat_identity(out, out->sector_count);
    if (error == RT_EOK) goto identified;
    if (error == -RT_ENOSYS || first[510] != 0x55 || first[511] != 0xaa)
        goto done;
    /* GPT, extended partitions and multiple populated MBR entries have no
     * unique supported format target in this component. */
    count = 0;
    start = 0;
    for (i = 0; i < 4; ++i)
    {
        const uint8_t *p = first + 446 + i * 16;
        if (!p[4] && !_le32(p + 8) && !_le32(p + 12)) continue;
        if (count || (p[0] != 0 && p[0] != 0x80) ||
            !(p[4] == 1 || p[4] == 4 || p[4] == 6 || p[4] == 0x0b ||
              p[4] == 0x0c || p[4] == 0x0e))
        {
            error = -RT_ENOSYS;
            goto done;
        }
        start = _le32(p + 8);
        count = _le32(p + 12);
        if (!start || !count || start >= out->sector_count ||
            count > out->sector_count - start)
        {
            error = -RT_ERROR;
            goto done;
        }
    }
    if (!count) { error = -RT_ERROR; goto done; }
    memcpy(out->partition_table, first + 438, sizeof(out->partition_table));
    out->volume_start = start;
    out->partition_sector_count = count;
    error = _identity_read(device, start, out->volume_boot);
    if (error != RT_EOK) goto done;
    error = _fat_identity(out, count);
    if (error != RT_EOK) goto done;
identified:
    out->version = SDCARD_IDENTITY_VERSION;
done:
    if (opened)
    {
        rt_err_t close_error = rt_device_close(device);
        if (close_error != RT_EOK) error = close_error;
    }
    if (!_guard_current()) error = -RT_EBUSY;
    if (error != RT_EOK) memset(out, 0, sizeof(*out));
    return error;
}

rt_err_t sdcard_port_format(struct rt_mmcsd_host *host, const char *device_name)
{
    rt_device_t device = rt_device_find(device_name);
    rt_err_t error;
    if (host != s_guard.host || !_guard_current()) return -RT_EBUSY;
    if (!device || device->ref_count) return -RT_EBUSY;
    rt_set_errno(0);
    error = dfs_mkfs("elm", device_name);
    if (error != RT_EOK)
    {
        rt_err_t saved_errno = rt_get_errno();
        if (error == -1 && saved_errno) return saved_errno < 0 ? saved_errno : -saved_errno;
        return error < 0 ? error : -RT_ERROR;
    }
    return _guard_current() ? RT_EOK : -RT_EBUSY;
}
