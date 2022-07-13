// Copyright 2021 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdint.h>
#include <string.h>
#include "soc/soc_memory_layout.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rtc_store.h"

/**
 * @brief Manages RTC store for critical and non_critical data
 *
 * @attention there are some prints in this file, (not logs). this is to avoid logging them in Insights,
 *    which may get stuck in recursive mutex etc. Please be careful if you are using logs...
 */

// #define RTC_STORE_DBG_PRINTS 1

#define DIAG_CRITICAL_BUF_SIZE        CONFIG_RTC_STORE_CRITICAL_DATA_SIZE
#define NON_CRITICAL_DATA_SIZE        (CONFIG_RTC_STORE_DATA_SIZE - DIAG_CRITICAL_BUF_SIZE)

/* If data is perfectly aligned then buffers get wrapped and we have to perform two read
 * operation to get all the data, +1 ensures that data will be moved to the start of buffer
 * when there is not enough space at the end of buffer.
 */
#if ((NON_CRITICAL_DATA_SIZE % 4) == 0)
#define DIAG_NON_CRITICAL_BUF_SIZE    (NON_CRITICAL_DATA_SIZE + 1)
#else
#define DIAG_NON_CRITICAL_BUF_SIZE    NON_CRITICAL_DATA_SIZE
#endif

/* When buffer is filled beyond configured capacity then we post an event.
 * In case of failure in sending data over the network, new critical data is dropped and
 * non-critical data is overwritten.
 */

/* When current free size of buffer drops below (100 - reporting_watermark)% then we post an event */
#define DIAG_CRITICAL_DATA_REPORTING_WATERMARK \
    ((DIAG_CRITICAL_BUF_SIZE * (100 - CONFIG_RTC_STORE_REPORTING_WATERMARK_PERCENT)) / 100)
#define DIAG_NON_CRITICAL_DATA_REPORTING_WATERMARK \
    ((DIAG_NON_CRITICAL_BUF_SIZE * (100 - CONFIG_RTC_STORE_REPORTING_WATERMARK_PERCENT)) / 100)

ESP_EVENT_DEFINE_BASE(RTC_STORE_EVENT);

// Assumption is RTC memory size will never exeed UINT16_MAX
typedef union {
    struct {
        uint16_t read_offset;
        uint16_t filled;
    };
    uint32_t value;
} data_store_info_t;

typedef struct {
    uint8_t *buf;
    size_t size;
    data_store_info_t info;
} data_store_t;

typedef struct {
    SemaphoreHandle_t lock;     // critical lock
    data_store_t *store;        // pointer to rtc data store
    size_t wrap_cnt;            // keep track of no. of times wrapping happened
} rbuf_data_t;

typedef struct {
    bool init;
    rbuf_data_t critical;
    rbuf_data_t non_critical;

} rtc_store_priv_data_t;

#define SHA_SIZE  (CONFIG_APP_RETRIEVE_LEN_ELF_SHA + 1)

/**
 * @brief header record to identify firmware/boot data a record represent
 */
typedef struct {
    // uint32_t magic;             // magic, identifies master hdr
    uint8_t gen_id;             // generated on each hard reset
    uint8_t boot_cnt;           // updated on each soft reboot
    char sha_sum[SHA_SIZE];     // elf shasum
    bool valid;                 //
} rtc_store_meta_header_t;

// have a strategy to invalidate data beyond this
//
#define RTC_STORE_MAX_META_RECORDS  (10) // Have 5 master records at max

// each data record must have an identifier to point a meta

typedef struct {
    struct {
        data_store_t store;
        uint8_t buf[DIAG_CRITICAL_BUF_SIZE];
    } critical;
    struct {
        data_store_t store;
        uint8_t buf[DIAG_NON_CRITICAL_BUF_SIZE];
    } non_critical;
    rtc_store_meta_header_t meta[RTC_STORE_MAX_META_RECORDS];
} rtc_store_t;

static rtc_store_priv_data_t s_priv_data;
RTC_NOINIT_ATTR static rtc_store_t s_rtc_store;

static inline size_t data_store_get_size(data_store_t *store)
{
    return store->size;
}

static inline size_t data_store_get_free_at_end(data_store_t *store)
{
    data_store_info_t *info = (data_store_info_t *) &store->info;
    return store->size - (info->filled + info->read_offset);
}

static inline size_t data_store_get_free(data_store_t *store)
{
    data_store_info_t *info = (data_store_info_t *) &store->info;
    return store->size - info->filled;
}

static inline size_t data_store_get_filled(data_store_t *store)
{
    data_store_info_t *info = (data_store_info_t *) &store->info;
    return info->filled;
}

static void rtc_store_read_complete(rbuf_data_t *rbuf_data, size_t len)
{
    data_store_info_t info =  {
        .value = rbuf_data->store->info.value,
    };
    // modify new pointers
    info.filled -= len;
    info.read_offset += len;

    if (((data_store_info_t *) &info)->read_offset > rbuf_data->store->size) {
        ((data_store_info_t *) &info)->read_offset -= rbuf_data->store->size;
        rbuf_data->wrap_cnt++; // wrap around count
    }

    // commit modifications
    rbuf_data->store->info.value = info.value;
}

static void rtc_store_write_complete(rbuf_data_t *rbuf_data, size_t len)
{
    data_store_info_t *info = (data_store_info_t *) &rbuf_data->store->info;
    info->filled += len;
}

static size_t rtc_store_write(rbuf_data_t *rbuf_data, void *data, size_t len)
{
    data_store_info_t *info = (data_store_info_t *) &rbuf_data->store->info;
#if RTC_STORE_DBG_PRINTS
    printf("rb_info: size %d, available: %d, filled %d, read_ptr %d, to_write %d\n",
                rbuf_data->store->size, data_store_get_free(rbuf_data->store),
                data_store_get_filled(rbuf_data->store), info->read_offset, len);
#endif

    uint16_t write_offset = info->filled + info->read_offset;
    if (write_offset > rbuf_data->store->size) { // wrap around
        write_offset -= rbuf_data->store->size;
    }


    void *write_ptr = (void *) (rbuf_data->store->buf + write_offset);
    size_t free_at_end = data_store_get_free_at_end(rbuf_data->store);
    if (free_at_end < len) {
        memcpy(write_ptr, data, free_at_end);
        memcpy(rbuf_data->store->buf, data + free_at_end, len - free_at_end);
    } else {
        memcpy(write_ptr, data, len);
    }

    rtc_store_write_complete(rbuf_data, len);
    return len;
}

esp_err_t rtc_store_critical_data_write(void *data, size_t len)
{
    esp_err_t ret = ESP_OK;

    if (!data || !len) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_priv_data.init) {
        printf("rtc_store init not done! skipping critical_data_write...\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (len > DIAG_CRITICAL_BUF_SIZE) {
        printf("rtc_store_critical_data_write: len too large %d, size %d\n",
                len, DIAG_CRITICAL_BUF_SIZE);
        return ESP_FAIL;
    }
    xSemaphoreTake(s_priv_data.critical.lock, portMAX_DELAY);

    size_t curr_free = data_store_get_free(s_priv_data.critical.store);
    size_t free_at_end = data_store_get_free_at_end(s_priv_data.critical.store);
    // If no space available... Raise write fail event
    if (curr_free < len) {
        esp_event_post(RTC_STORE_EVENT, RTC_STORE_EVENT_CRITICAL_DATA_WRITE_FAIL, data, len, 0);
        printf("%s, curr_free %d, req_free %d\n", "rtc_store", curr_free, len);
        ret = ESP_ERR_NO_MEM;
    } else { // we have enough space
        rtc_store_write(&s_priv_data.critical, data, len);
    }

    curr_free = data_store_get_free(s_priv_data.critical.store);

    xSemaphoreGive(s_priv_data.critical.lock);

    if (curr_free < DIAG_CRITICAL_DATA_REPORTING_WATERMARK) {
        esp_event_post(RTC_STORE_EVENT, RTC_STORE_EVENT_CRITICAL_DATA_LOW_MEM, NULL, 0, 0);
    }
    return ret;
}

esp_err_t rtc_store_non_critical_data_write(const char *dg, void *data, size_t len)
{
    if (!dg || !len || !data) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!esp_ptr_in_drom(dg)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_priv_data.init) {
        printf("rtc_store init not done! skipping non_critical_data_write...\n");
        return ESP_ERR_INVALID_STATE;
    }
    rtc_store_non_critical_data_hdr_t header;
    size_t req_free = sizeof(header) + len;
    size_t curr_free;

    if (req_free > DIAG_NON_CRITICAL_BUF_SIZE) {
        printf("rtc_store_non_critical_data_write: len too large %d, size %d\n",
                req_free, DIAG_NON_CRITICAL_BUF_SIZE);
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_priv_data.non_critical.lock, 0) == pdFALSE) {
        return ESP_FAIL;
    }

#if CONFIG_RTC_STORE_OVERWRITE_NON_CRITICAL_DATA
    data_store_info_t *info = (data_store_info_t *) &s_priv_data.non_critical.store->info;
    /* Make enough room for the item */
    while (data_store_get_free(s_priv_data.non_critical.store) < req_free) {
        uint8_t *read_ptr = s_priv_data.non_critical.store->buf + info->read_offset;
        memcpy(&header, read_ptr, sizeof(header));
        rtc_store_read_complete(&s_priv_data.non_critical, sizeof(header) + header.len);
    }
#else // just check if we have enough space to write the item
    curr_free = data_store_get_free(s_priv_data.non_critical.store);
    if (curr_free < req_free) {
        printf("%s, curr_free %d, req_free %d\n", "rtc_store", curr_free, req_free);
        xSemaphoreGive(s_priv_data.non_critical.lock);
        esp_event_post(RTC_STORE_EVENT, RTC_STORE_EVENT_NON_CRITICAL_DATA_LOW_MEM, NULL, 0, 0);
        return ESP_ERR_NO_MEM;
    }
#endif

    memset(&header, 0, sizeof(header));
    header.dg = dg;
    header.len = len;

    // we have made sure of free size at this point
    rtc_store_write(&s_priv_data.non_critical, &header, sizeof(header));
    rtc_store_write(&s_priv_data.non_critical, data, len);

    curr_free = data_store_get_free(s_priv_data.non_critical.store);
    xSemaphoreGive(s_priv_data.non_critical.lock);

    // Post low memory event even if data overwrite is enabled.
    if (curr_free < DIAG_NON_CRITICAL_DATA_REPORTING_WATERMARK) {
        esp_event_post(RTC_STORE_EVENT, RTC_STORE_EVENT_NON_CRITICAL_DATA_LOW_MEM, NULL, 0, 0);
    }
    return ESP_OK;
}

static int rtc_store_data_read(rbuf_data_t *rbuf_data, uint8_t *buf, size_t size)
{
    if (!size) {
        return -1;
    }
    if (!s_priv_data.init) {
        return -1;
    }

    xSemaphoreTake(rbuf_data->lock, portMAX_DELAY);
    data_store_info_t *info = (data_store_info_t *) &rbuf_data->store->info;

    if (info->filled < size) {
        size = info->filled;
    }
    int free_at_end = data_store_get_free_at_end(rbuf_data->store);
    if (free_at_end < size) {
        // data is wrapped, read data in 2 parts
        memcpy(buf, rbuf_data->store->buf + info->read_offset, free_at_end);
        memcpy(buf + free_at_end, rbuf_data->store->buf, size - free_at_end);
    } else {
        // single memcpy
        memcpy(buf, rbuf_data->store->buf + info->read_offset, size);
    }
    xSemaphoreGive(rbuf_data->lock);
    return size;
}

static esp_err_t rtc_store_data_release(rbuf_data_t *rbuf_data, size_t size)
{
    if (!s_priv_data.init) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(rbuf_data->lock, portMAX_DELAY);
    rtc_store_read_complete(rbuf_data, size);
    xSemaphoreGive(rbuf_data->lock);
    return ESP_OK;
}

int rtc_store_critical_data_read(uint8_t *buf, size_t size)
{
    return rtc_store_data_read(&s_priv_data.critical, buf, size);
}

int rtc_store_critical_data_read_and_release(uint8_t *buf, size_t size)
{
    int data_read = rtc_store_data_read(&s_priv_data.critical, buf, size);
    if (data_read > 0) {
        rtc_store_data_release(&s_priv_data.critical, size);
    }
    return data_read;
}

int rtc_store_non_critical_data_read(uint8_t *buf, size_t size)
{
    return rtc_store_data_read(&s_priv_data.non_critical, buf, size);
}

int rtc_store_non_critical_data_read_and_release(uint8_t *buf, size_t size)
{
    int data_read = rtc_store_data_read(&s_priv_data.non_critical, buf, size);
    if (data_read > 0) {
        rtc_store_data_release(&s_priv_data.non_critical, data_read);
    }
    return data_read;
}

esp_err_t rtc_store_critical_data_release(size_t size)
{
    return rtc_store_data_release(&s_priv_data.critical, size);
}

esp_err_t rtc_store_non_critical_data_release(size_t size)
{
    return rtc_store_data_release(&s_priv_data.non_critical, size);
}

static void rtc_store_rbuf_deinit(rbuf_data_t *rbuf_data)
{
    if (rbuf_data->lock) {
        vSemaphoreDelete(rbuf_data->lock);
        rbuf_data->lock = NULL;
    }
}

void rtc_store_deinit(void)
{
    rtc_store_rbuf_deinit(&s_priv_data.critical);
    rtc_store_rbuf_deinit(&s_priv_data.non_critical);
    s_priv_data.init = false;
}

static bool rtc_store_integrity_check(data_store_t *store)
{
    data_store_info_t *info = (data_store_info_t *) &store->info;
    if (info->filled > store->size ||
            info->read_offset > store->size ||
            (info->read_offset + info->filled) > store->size) {
        return false;
    }
    return true;
}

static esp_err_t rtc_store_rbuf_init(rbuf_data_t *rbuf_data,
                                     data_store_t *rtc_store,
                                     uint8_t *rtc_buf,
                                     size_t rtc_buf_size)
{
    esp_reset_reason_t reset_reason = esp_reset_reason();

    rbuf_data->lock = xSemaphoreCreateMutex();
    if (!rbuf_data->lock) {
#if RTC_STORE_DBG_PRINTS
        printf("rtc_store_rbuf_init: lock creation failed\n");
#endif
        return ESP_ERR_NO_MEM;
    }

    /* Check for stale data */
    if (reset_reason == ESP_RST_UNKNOWN ||
            reset_reason == ESP_RST_POWERON ||
            reset_reason == ESP_RST_BROWNOUT) {
        memset(rtc_store, 0, sizeof(data_store_t));
        memset(rtc_buf, 0, rtc_buf_size);
    }

    /* Point priv_data to actual RTC data */
    rbuf_data->store = rtc_store;
    rbuf_data->store->buf = rtc_buf;
    rbuf_data->store->size = rtc_buf_size;

    if (rtc_store_integrity_check(rtc_store) == false) {
        // discard all the existing data
        printf("%s: intergrity_check failed, discarding old data...\n", "rtc_store");
        rtc_store->info.value = 0;
    }
    return ESP_OK;
}

esp_err_t rtc_store_init(void)
{
    esp_err_t err;
    if (s_priv_data.init) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Initialize critical RTC rbuf */
    err = rtc_store_rbuf_init(&s_priv_data.critical,
                              &s_rtc_store.critical.store,
                              s_rtc_store.critical.buf,
                              DIAG_CRITICAL_BUF_SIZE);
    if (err != ESP_OK) {
#if RTC_STORE_DBG_PRINTS
        printf("rtc_store_rbuf_init(critical) failed\n");
#endif
        return err;
    }
    /* Initialize non critical RTC rbuf */
    err = rtc_store_rbuf_init(&s_priv_data.non_critical,
                              &s_rtc_store.non_critical.store,
                              s_rtc_store.non_critical.buf,
                              DIAG_NON_CRITICAL_BUF_SIZE);
    if (err != ESP_OK) {
#if RTC_STORE_DBG_PRINTS
        printf("rtc_store_rbuf_init(non_critical) failed\n");
#endif
        rtc_store_rbuf_deinit(&s_priv_data.critical);
        return err;
    }
    s_priv_data.init = true;
    return ESP_OK;
}
