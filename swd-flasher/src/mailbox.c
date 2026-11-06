#include "mailbox.h"
#include <string.h>

int mbx_read(swd_t *s, mbx_t *m)
{
    uint32_t w[MBX_WORDS];
    int rc = swd_mem_read_block(s, MBX_ADDR, w, MBX_WORDS);
    if (rc != SWD_OK) return rc;
    memcpy(m, w, sizeof *m);   // every field is a little-endian 32-bit word, same as the target
    return SWD_OK;
}

int mbx_write_command(swd_t *s, uint32_t heartbeat, int32_t cmd_master, int32_t cmd_slave, uint32_t enable)
{
    int rc;
    if ((rc = swd_mem_write32(s, MBX_ADDR + MBX_OFF_CMD_MASTER, (uint32_t)cmd_master)) != SWD_OK) return rc;
    if ((rc = swd_mem_write32(s, MBX_ADDR + MBX_OFF_CMD_SLAVE, (uint32_t)cmd_slave)) != SWD_OK) return rc;
    if ((rc = swd_mem_write32(s, MBX_ADDR + MBX_OFF_ENABLE, enable)) != SWD_OK) return rc;
    return swd_mem_write32(s, MBX_ADDR + MBX_OFF_HEARTBEAT, heartbeat);
}

int mbx_write_led(swd_t *s, uint32_t master_mask, uint32_t slave_mask)
{
    int rc;
    if ((rc = swd_mem_write32(s, MBX_ADDR + MBX_OFF_LED_MASTER, master_mask & MBX_LED_ALL)) != SWD_OK) return rc;
    return swd_mem_write32(s, MBX_ADDR + MBX_OFF_LED_SLAVE, slave_mask & MBX_LED_ALL);
}

int mbx_power_off(swd_t *s)
{
    return swd_mem_write32(s, MBX_ADDR + MBX_OFF_POWER, MBX_POWER_OFF_KEY);
}
