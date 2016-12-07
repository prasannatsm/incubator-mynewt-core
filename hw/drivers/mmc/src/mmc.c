/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <hal/hal_spi.h>
#include <hal/hal_gpio.h>

#include <mmc/mmc.h>

#include <stdio.h>

#define MIN(n, m) (((n) < (m)) ? (n) : (m))

/* MMC commands used by the driver */
#define CMD0                (0)            /* GO_IDLE_STATE */
#define CMD1                (1)            /* SEND_OP_COND (MMC) */
#define CMD8                (8)            /* SEND_IF_COND */
#define CMD12               (12)           /* STOP_TRANSMISSION */
#define CMD16               (16)           /* SET_BLOCKLEN */
#define CMD17               (17)           /* READ_SINGLE_BLOCK */
#define CMD18               (18)           /* READ_MULTIPLE_BLOCK */
#define CMD24               (24)           /* WRITE_BLOCK */
#define CMD25               (25)           /* WRITE_MULTIPLE_BLOCK */
#define CMD55               (55)           /* APP_CMD */
#define CMD58               (58)           /* READ_OCR */
#define ACMD41              (0x80 + 41)    /* SEND_OP_COND (SDC) */

#define HCS                 ((uint32_t) 1 << 30)

/* Response types */
#define R1                  (0)
#define R1b                 (1)
#define R2                  (2)
#define R3                  (3)            /* CMD58 */
#define R7                  (4)            /* CMD8 */

/* R1 response status */
#define R_IDLE              (0x01)
#define R_ERASE_RESET       (0x02)
#define R_ILLEGAL_COMMAND   (0x04)
#define R_CRC_ERROR         (0x08)
#define R_ERASE_ERROR       (0x10)
#define R_ADDR_ERROR        (0x20)
#define R_PARAM_ERROR       (0x40)

#define BLOCK_LEN           (512)

static uint8_t g_block_buf[BLOCK_LEN];

static struct hal_spi_settings mmc_settings = {
    .data_order = HAL_SPI_MSB_FIRST,
    .data_mode  = HAL_SPI_MODE0,
    /* XXX: MMC initialization accepts clocks in the range 100-400KHz */
    /* TODO: switch to high-speed aka 25MHz after initialization. */
    .baudrate   = 100,
    .word_size  = HAL_SPI_WORD_SIZE_8BIT,
};

/* FIXME: this limits usage to single MMC spi device */
static struct mmc_cfg {
    int                      spi_num;
    int                      ss_pin;
    void                     *spi_cfg;
    struct hal_spi_settings  *settings;
} g_mmc_cfg;

static int
mmc_error_by_status(uint8_t status)
{
    if (status == 0) {
        return MMC_OK;
    } else if (status & R_IDLE) {
        return MMC_TIMEOUT;
    } else if (status & R_ERASE_RESET) {
        /* TODO */
    } else if (status & R_ILLEGAL_COMMAND) {
        /* TODO */
    } else if (status & R_CRC_ERROR) {
        return MMC_CRC_ERROR;
    } else if (status & R_ERASE_ERROR) {
        /* TODO */
    } else if (status & R_ADDR_ERROR) {
        /* TODO */
    } else if (status & R_PARAM_ERROR) {
        return MMC_PARAM_ERROR;
    }

    return MMC_CARD_ERROR;
}

static struct mmc_cfg *
mmc_cfg_dev(uint8_t id)
{
    if (id != 0) {
        return NULL;
    }

    return &g_mmc_cfg;
}

static uint8_t
send_mmc_cmd(struct mmc_cfg *mmc, uint8_t cmd, uint32_t payload)
{
    int n;
    uint8_t status;
    uint8_t crc;

    if (cmd & 0x80) {
        /* TODO: refactor recursion? */
        /* TODO: error checking */
        send_mmc_cmd(mmc, CMD55, 0);
    }

    /* 4.7.2: Command Format */
    hal_spi_tx_val(mmc->spi_num, 0x40 | (cmd & ~0x80));

    hal_spi_tx_val(mmc->spi_num, payload >> 24 & 0xff);
    hal_spi_tx_val(mmc->spi_num, payload >> 16 & 0xff);
    hal_spi_tx_val(mmc->spi_num, payload >>  8 & 0xff);
    hal_spi_tx_val(mmc->spi_num, payload       & 0xff);

    /**
     * 7.2.2 Bus Transfer Protection
     *   NOTE: SD is in CRC off mode by default but CMD0 and CMD8 always
     *   require a valid CRC.
     *   NOTE: CRC can be turned on with CMD59 (CRC_ON_OFF).
     */
    crc = 0x01;
    switch (cmd) {
        case CMD0:
            crc = 0x95;
            break;
        case CMD8:
            crc = 0x87;
            break;
    }
    hal_spi_tx_val(mmc->spi_num, crc);

    //printf("==> sending cmd %d\n", cmd & ~0x80);

    for (n = 255; n > 0; n--) {
        status = hal_spi_tx_val(mmc->spi_num, 0xff);
        if ((status & 0x80) == 0) break;
        //os_time_delay(OS_TICKS_PER_SEC / 1000);
    }

    if (n == 0) {
        return status;
    }

    return status;
}

/**
 * NOTE:
 */

/**
 * Initialize the MMC driver
 *
 * @param spi_num Number of the SPI channel to be used by MMC
 * @param spi_cfg Low-level device specific SPI configuration
 * @param ss_pin Number of SS pin if SW controlled, -1 otherwise
 * @param mmc_spi_cfg 
 *
 * @return 0 on success, non-zero on failure
 */
int
mmc_init(int spi_num, void *spi_cfg, int ss_pin)
{
    int rc;
    int i;
    uint8_t status;
    uint8_t cmd_resp[4];
    uint32_t ocr;
    os_time_t timeout;
    struct mmc_cfg *mmc;

    /* TODO: create new struct for every new spi mmc, add to SLIST */
    mmc = &g_mmc_cfg;
    mmc->spi_num = spi_num;
    mmc->ss_pin = ss_pin;
    mmc->spi_cfg = spi_cfg;
    mmc->settings = &mmc_settings;

    hal_gpio_init_out(mmc->ss_pin, 1);

    rc = hal_spi_init(mmc->spi_num, mmc->spi_cfg, HAL_SPI_TYPE_MASTER);
    if (rc) {
        return (rc);
    }

    rc = hal_spi_config(mmc->spi_num, mmc->settings);
    if (rc) {
        return (rc);
    }

    hal_spi_set_txrx_cb(mmc->spi_num, NULL, NULL);
    hal_spi_enable(mmc->spi_num);

    /**
     * NOTE: The state machine below follows:
     *       Section 6.4.1: Power Up Sequence for SD Bus Interface.
     *       Section 7.2.1: Mode Selection and Initialization.
     */

    /* give 10ms for VDD rampup */
    os_time_delay(OS_TICKS_PER_SEC / 100);

    hal_gpio_write(mmc->ss_pin, 0);
    hal_spi_tx_val(mmc->spi_num, 0xff);

    /* send the required >= 74 clock cycles */
    for (i = 0; i < 74; i++) {
        hal_spi_tx_val(mmc->spi_num, 0xff);
    }

    /* put card in idle state */
    status = send_mmc_cmd(mmc, CMD0, 0);

    /* No card inserted or bad card? */
    if (status != R_IDLE) {
        rc = mmc_error_by_status(status);
        goto out;
    }

    /**
     * 4.3.13: Ask for 2.7-3.3V range, send 0xAA pattern
     *
     * NOTE: cards that are not compliant with "Physical Spec Version 2.00"
     *       will answer this with R_ILLEGAL_COMMAND.
     */
    status = send_mmc_cmd(mmc, CMD8, 0x1AA);
    for (i = 0; i < 4; i++) {
        cmd_resp[i] = (uint8_t) hal_spi_tx_val(mmc->spi_num, 0xff);
    }

    if (status & R_ILLEGAL_COMMAND) {
        /* Ver1.x SD Memory Card or Not SD Memory Card */

        ocr = send_mmc_cmd(mmc, CMD58, 0);

        /* TODO: check if voltage range is ok! */

        if (ocr & R_ILLEGAL_COMMAND) {

        }

        /* TODO: set blocklen */

    } else {

        /**
         * Ver2.00 or later SD Memory Card
         */

        if (cmd_resp[3] != 0xAA) {
            rc = MMC_RESPONSE_ERROR;
            goto out;
        }

        /**
         * 4.3.13 Send Interface Condition Command (CMD8)
         *   Check VHS for 2.7-3.6V support
         */
        if (cmd_resp[2] != 0x01) {
            rc = MMC_VOLTAGE_ERROR;
            goto out;
        }

        /**
         * Wait for card to leave IDLE state or time out
         */

        timeout = os_time_get() + OS_TICKS_PER_SEC;
        for (;;) {
            status = send_mmc_cmd(mmc, ACMD41, HCS);
            if ((status & R_IDLE) == 0 || os_time_get() > timeout) {
                break;
            }
            os_time_delay(OS_TICKS_PER_SEC / 10);
        }

        if (status) {
            rc = mmc_error_by_status(status);
            goto out;
        }

        /**
         * Check if this is an high density card
         */

        status = send_mmc_cmd(mmc, CMD58, 0);
        for (i = 0; i < 4; i++) {
            cmd_resp[i] = (uint8_t) hal_spi_tx_val(mmc->spi_num, 0xff);
        }
        if (status == 0 && (cmd_resp[0] & (1 << 6))) {  /* FIXME: CCS */
            /**
             * TODO: if CCS is set this is an SDHC or SDXC card!
             *       SDSC uses byte addressing, SDHC/SDXC block addressing
             */
        }

#if 0  /* TODO: Can check all possible voltages supported by card */
    printf("Card supports: ");
    if (ocr & (1 << 15)) {
        printf("2.7-2.8 ");
    }
#endif

    }

out:
    hal_gpio_write(mmc->ss_pin, 1);
    return rc;
}

/**
 * @return 0 on success, non-zero on failure
 */
int
mmc_read(uint8_t mmc_id, uint32_t addr, void *buf, size_t len)
{
    uint8_t cmd;
    uint8_t res;
    uint32_t n;
    size_t block_len;
    size_t block_count;
    os_time_t timeout;
    uint32_t block_addr;
    size_t offset;
    size_t index;
    size_t amount;
    struct mmc_cfg *mmc;

    mmc = mmc_cfg_dev(mmc_id);
    if (mmc == NULL) {
        return (MMC_DEVICE_ERROR);
    }

    block_len = (len + BLOCK_LEN - 1) & ~(BLOCK_LEN - 1);
    block_count = block_len / BLOCK_LEN;
    block_addr = addr / BLOCK_LEN;
    offset = addr - (block_addr * BLOCK_LEN);

    //printf("addr=0x%lx, block_addr=0x%lx, offset=%d\n", addr, block_addr, offset);
    //printf("len=%d, block_len=%d, block_count=%d\n", len, block_len, block_count);

    hal_gpio_write(mmc->ss_pin, 0);

    cmd = (block_count == 1) ? CMD17 : CMD18;
    res = send_mmc_cmd(mmc, cmd, block_addr);
    if (res != MMC_OK) {
        hal_gpio_write(mmc->ss_pin, 1);
        return (MMC_CARD_ERROR);
    }

    /**
     * 7.3.3 Control tokens
     *   Wait up to 200ms for control token.
     */
    timeout = os_time_get() + OS_TICKS_PER_SEC / 5;
    do {
        res = hal_spi_tx_val(mmc->spi_num, 0xff);
        if (res != 0xFF) break;
        os_time_delay(OS_TICKS_PER_SEC / 20);
    } while (os_time_get() < timeout);

    /**
     * 7.3.3.2 Start Block Tokens and Stop Tran Token
     */
    if (res == 0xFE) {
        index = 0;
        while (block_count--) {
            /**
             * FIXME: on last run doesn't need to transfer all BLOCK_LEN bytes!
             */

            for (n = 0; n < BLOCK_LEN; n++) {
                g_block_buf[n] = hal_spi_tx_val(mmc->spi_num, 0xff);
            }

            /* FIXME: consume CRC-16, but should check */
            hal_spi_tx_val(mmc->spi_num, 0xff);
            hal_spi_tx_val(mmc->spi_num, 0xff);

            amount = MIN(BLOCK_LEN - offset, len);

            //printf("copying %d bytes to index %d\n", amount, index);
            memcpy(((uint8_t *)buf + index), &g_block_buf[offset], amount);

            offset = 0;
            len -= amount;
            index += amount;
        }

        if (cmd == CMD18) {
            send_mmc_cmd(mmc, CMD12, 0);  /* FIXME */
        }

        hal_gpio_write(mmc->ss_pin, 1);

        return MMC_OK;
    }

    hal_gpio_write(mmc->ss_pin, 1);

    return MMC_CARD_ERROR;
}

/**
 * @return 0 on success, non-zero on failure
 */
int
mmc_write(uint8_t mmc_id, uint32_t addr, const void *buf, size_t len)
{
    uint8_t cmd;
    uint8_t res;
    uint32_t n;
    size_t block_len;
    size_t block_count;
    os_time_t timeout;
    uint32_t block_addr;
    size_t offset;
    size_t index;
    size_t amount;
    int status;
    struct mmc_cfg *mmc;

    mmc = mmc_cfg_dev(mmc_id);
    if (mmc == NULL) {
        return (MMC_DEVICE_ERROR);
    }

    block_len = (len + BLOCK_LEN - 1) & ~(BLOCK_LEN - 1);
    block_count = block_len / BLOCK_LEN;
    block_addr = addr / BLOCK_LEN;
    offset = addr - (block_addr * BLOCK_LEN);

    hal_gpio_write(mmc->ss_pin, 0);

    /**
     * This code ensures that if the requested address doesn't align with the
     * beginning address of a sector, the initial bytes are first read to the
     * buffer to be then written later.
     *
     * NOTE: this code will never run when using a FS that is sector addressed
     * like FAT (offset is always 0).
     */
    if (offset) {
        res = send_mmc_cmd(mmc, CMD17, block_addr);
        if (res != MMC_OK) {
            hal_gpio_write(mmc->ss_pin, 1);
            return (MMC_CARD_ERROR);
        }

        timeout = os_time_get() + OS_TICKS_PER_SEC / 5;
        do {
            res = hal_spi_tx_val(mmc->spi_num, 0xff);
            if (res != 0xff) break;
            os_time_delay(OS_TICKS_PER_SEC / 20);
        } while (os_time_get() < timeout);

        if (res != 0xfe) {
            hal_gpio_write(mmc->ss_pin, 1);
            return (MMC_CARD_ERROR);
        }

        for (n = 0; n < BLOCK_LEN; n++) {
            g_block_buf[n] = hal_spi_tx_val(mmc->spi_num, 0xff);
        }

        hal_spi_tx_val(mmc->spi_num, 0xff);
        hal_spi_tx_val(mmc->spi_num, 0xff);
    }

    /* now start write */

    cmd = (block_count == 1) ? CMD24 : CMD25;
    res = send_mmc_cmd(mmc, cmd, block_addr);
    if (res != MMC_OK) {
        hal_gpio_write(mmc->ss_pin, 1);
        return (MMC_CARD_ERROR);
    }

    /* FIXME: one byte gap, is this really required? */
    hal_spi_tx_val(mmc->spi_num, 0xff);

    index = 0;
    do {
        /* FIXME: must wait for ready here? */

        /**
         * 7.3.3.2 Start Block Tokens and Stop Tran Token
         */
        if (cmd == CMD24) {
            hal_spi_tx_val(mmc->spi_num, 0xFE);
        } else {
            hal_spi_tx_val(mmc->spi_num, 0xFC);
        }

        amount = MIN(BLOCK_LEN - offset, len);
        memcpy(&g_block_buf[offset], ((uint8_t *)buf + index), amount);

        for (n = 0; n < BLOCK_LEN; n++) {
            hal_spi_tx_val(mmc->spi_num, g_block_buf[n]);
        }

        /* CRC */
        hal_spi_tx_val(mmc->spi_num, 0xff);
        hal_spi_tx_val(mmc->spi_num, 0xff);

        /**
         * 7.3.3.1 Data Response Token
         */
        res = hal_spi_tx_val(mmc->spi_num, 0xff);
        if ((res & 0x1f) != 0x05) {
            break;
        }

        offset = 0;
        len -= amount;
        index += amount;

    } while (len);

    /* TODO: send stop tran token */
    if (cmd == CMD25) {
    }

    res &= 0x1f;
    switch (res) {
        case 0x05:
            status = MMC_OK;
            break;
        case 0x0b:
            status = MMC_CRC_ERROR;
            break;
        case 0x0d:  /* passthrough */
        default:
            status = MMC_WRITE_ERROR;
    }

    timeout = os_time_get() + 5 * OS_TICKS_PER_SEC;
    do {
        res = hal_spi_tx_val(mmc->spi_num, 0xff);
        if (res) break;
        os_time_delay(OS_TICKS_PER_SEC / 100);
    } while (os_time_get() < timeout);

    hal_gpio_write(mmc->ss_pin, 1);

    return status;
}