#include <rtconfig.h>
#include <bf0_hal.h>
#include <string.h>

#if defined(SOC_BF0_HCPU) && defined(BLUETOOTH) && !defined(CFG_BOOTLOADER)

void lcpu_rom_config_default(void);

static const uint16_t g_em_offset[HAL_LCPU_CONFIG_EM_BUF_MAX_NUM] =
{
    0x178, 0x178, 0x740, 0x7A0, 0x848, 0x8B8, 0xB38, 0xCE8, 0xE80, 0x1474, 0x14DC,
    0x1AF4, 0x22F4, 0x22F4, 0x22F4, 0x22F4, 0x22F4, 0x22F4, 0x22F4, 0x22F4, 0x25F4,
    0x2614, 0x268C, 0x26DC, 0x27FC, 0x2810, 0x2824, 0x2914, 0x2924, 0x29E4, 0x2A00,
    0x3A10, 0x4E24, 0x5F04,
};

void lcpu_rom_config(void)
{
    hal_lcpu_bluetooth_em_config_t em_offset = {0};
    hal_lcpu_bluetooth_act_configt_t act_cfg = {0};

    lcpu_rom_config_default();
    memcpy(em_offset.em_buf, g_em_offset, sizeof(g_em_offset));
    em_offset.is_valid = 1;
    HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_EM_BUF, &em_offset, sizeof(em_offset));

    act_cfg.bt_max_acl = 3;
    act_cfg.bt_max_sco = 2;
    act_cfg.ble_max_iso = 0;
    act_cfg.bit_valid = (1 << 4) | (1 << 1) | (1 << 0);
    HAL_LCPU_CONFIG_set(HAL_LCPU_CONFIG_BT_ACT_CFG, &act_cfg, sizeof(act_cfg));
}

#endif
