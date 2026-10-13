/* Coordinación: ventanas, pools, tareas secuencial/paralela y parada.
 * Accede al sensor por bmi270_sensor.h y al envío por imu_transport.h. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "imu_stream.h"
#include "imu_output.h"
#include "imu_wire.h"
#include "imu_wifi.h"
#include "imu_transport.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_err.h"
#include "esp_log.h"


#include "bmi270_sensor.h"
#include "imu_acquisition.h"
static const char *TAG="imu_acquisition";
static imu_config_t s_runtime;
#define MAX_BLOCKS 4
#define BLOCKS_PER_STREAM (s_runtime.blocks)
#define IMU_WINDOW_US ((int64_t)s_runtime.interval_ms * 1000LL)
#define FIFO_POLL_MS 10
#define FIFO_READ_CHUNK 512
#define ACC_ODR (s_runtime.accel_hz)
#define GYR_ODR (s_runtime.gyro_hz)

/* Independent pools: a slow consumer never modifies the active block.
 * Total blocks per stream and each capacity are validated before acquisition. */
static imu_block_t s_blocks[2][MAX_BLOCKS];
static void *s_storage[2][MAX_BLOCKS];
static uint64_t s_device_id;
static uint64_t s_boot_id;
static QueueHandle_t s_free[2];
static QueueHandle_t s_ready;
static imu_block_t *s_active[2];
static uint32_t s_sample_seq[2];
static uint32_t s_collected[2];
static uint32_t s_dropped_blocks[2];
static uint32_t s_window_seq;
static int64_t s_window_start;


static bool rate_supported(int hz)
{
    return hz == 25 || hz == 50 || hz == 100 || hz == 200 || hz == 400;
}

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    return bmi270_sensor_write_register(reg,value);
}

static esp_err_t fifo_start(void)
{
    esp_err_t err;
    /* Disable FIFO, filtered input at each sensor's own ODR, header mode,
     * stop-on-full and no synthetic sensortime/overread appended to bursts. */
    if ((err = write_reg(0x49, 0x00)) != ESP_OK ||
        (err = write_reg(0x45, 0x88)) != ESP_OK ||
        (err = write_reg(0x48, 0x01)) != ESP_OK ||
        (err = write_reg(0x7e, 0xb0)) != ESP_OK ||
        (err = write_reg(0x49, 0xd0)) != ESP_OK) return err;
    return ESP_OK;
}

static void start_block(imu_block_t *block, imu_stream_id_t stream)
{
    imu_block_init(block, stream, stream == IMU_ACCEL ? ACC_ODR : GYR_ODR,
                   stream == IMU_ACCEL ? s_runtime.accel_range : s_runtime.gyro_range,
                   s_window_seq, s_window_start, s_dropped_blocks[stream],
                   s_runtime.interval_ms);
}

static void rotate_windows(int64_t now)
{
    if (now - s_window_start < IMU_WINDOW_US) return;
    if (!s_runtime.parallel) {
        write_reg(0x49, 0x00);
        for (int stream=0;stream<2;stream++) {
            imu_block_t *block=s_active[stream];
            block->window_end_us=now;
            block->flags |= IMU_WIRE_FLAG_PARTIAL_WINDOW | IMU_WIRE_FLAG_SEQUENTIAL;
            if (block->count) {
                imu_transport_pending_add(1);
                esp_err_t err=imu_send_block(block);
                if(err!=ESP_OK)ESP_LOGE(TAG,"Salida secuencial fallo: %s",esp_err_to_name(err));
                imu_transport_pending_add(-1);
            }
        }
        s_window_seq++;
        s_window_start=esp_timer_get_time();
        for(int stream=0;stream<2;stream++)start_block(s_active[stream],(imu_stream_id_t)stream);
        imu_transport_window(s_window_seq);
        if(imu_transport_capture_requested())ESP_ERROR_CHECK(fifo_start());
        return;
    }
    uint32_t elapsed = (uint32_t)((now - s_window_start) / IMU_WINDOW_US);
    int64_t old_end = s_window_start + IMU_WINDOW_US;
    s_window_start += (int64_t)elapsed * IMU_WINDOW_US;
    s_window_seq += elapsed;
    imu_transport_window(s_window_seq);
    for (int stream = 0; stream < 2; stream++) {
        imu_block_t *completed = s_active[stream];
        imu_block_t *replacement = NULL;
        completed->window_end_us = old_end;
        /* Missed entire windows are explicit; never generate fake samples. */
        s_dropped_blocks[stream] += elapsed - 1;
        if (xQueueReceive(s_free[stream], &replacement, 0) == pdTRUE) {
            imu_transport_pending_add(1);
            if (xQueueSend(s_ready, &completed, 0) != pdTRUE) {
                imu_transport_pending_add(-1);
                s_dropped_blocks[stream]++;
                xQueueSend(s_free[stream], &replacement, 0);
                replacement = completed;
            }
        } else {
            /* No free output slot: discard this complete window, keep sampling. */
            s_dropped_blocks[stream]++;
            replacement = completed;
        }
        start_block(replacement, (imu_stream_id_t)stream);
        s_active[stream] = replacement;
    }
}

static void on_fifo_sample(void *context, imu_stream_id_t stream, const int16_t xyz[3])
{
    const uint64_t read_us = *(const uint64_t *)context;
    uint32_t target=stream==IMU_ACCEL?s_runtime.accel_samples:s_runtime.gyro_samples;
    if (!target || s_collected[stream]<target) {
        if(imu_block_append(s_active[stream],xyz,read_us,s_sample_seq[stream]++))s_collected[stream]++;
    }
    imu_transport_preview(stream,xyz,s_collected[stream],read_us/1000);
}

static esp_err_t drain_fifo(void)
{
    uint8_t reg = 0x24;
    uint8_t length_bytes[2];
    esp_err_t err = bmi270_sensor_read_registers(reg,length_bytes,2);
    if (err != ESP_OK) return err;
    size_t available = length_bytes[0] | ((size_t)(length_bytes[1] & 0x3f) << 8);
    if (!available) return ESP_OK;
    uint8_t bytes[FIFO_READ_CHUNK];
    size_t requested = available < sizeof(bytes) ? available : sizeof(bytes);
    reg = 0x26;
    err = bmi270_sensor_read_registers(reg,bytes,requested);
    if (err != ESP_OK) {
        /* A failed FIFO transaction may already have consumed bytes.
         * Flush to regain a known frame boundary; count unknown loss. */
        for (int i = 0; i < 2; i++) s_active[i]->fifo_errors++;
        esp_err_t flush_err = write_reg(0x7e, 0xb0);
        return flush_err != ESP_OK ? flush_err : err;
    }
    if(s_runtime.parallel)rotate_windows(esp_timer_get_time());
    uint64_t read_us = (uint64_t)esp_timer_get_time();
    imu_fifo_result_t result = imu_fifo_parse(bytes, requested, on_fifo_sample, &read_us);
    for (int i = 0; i < 2; i++) {
        /* Skips are FIFO FRAMES, not exact lost samples for each stream. */
        s_active[i]->fifo_skipped_frames += result.skipped_frames;
        if (result.invalid_header) s_active[i]->fifo_errors++;
    }
    if (result.invalid_header) return write_reg(0x7e, 0xb0);
    /* Partial frame bytes are discarded here: the sensor repeats that entire
     * frame on the next access. Keep draining on the next 10-ms iteration. */
    return ESP_OK;
}

/* Acquisition owns all FIFO and active-buffer mutations, including STOP. */
static bool stop_capture(bool fault)
{
    if (write_reg(0x49, 0x00) != ESP_OK) fault = true;
    /* Drain samples already captured, with no new FIFO input. */
    for (int n = 0; n < 8; n++) {
        if (drain_fifo() != ESP_OK) { fault = true; break; }
    }
    int64_t end = esp_timer_get_time();
    for (int stream = 0; stream < 2; stream++) {
        imu_block_t *completed = s_active[stream], *replacement = NULL;
        if (!completed->count) continue;
        if(!s_runtime.parallel) {
            completed->flags |= IMU_WIRE_FLAG_PARTIAL_WINDOW | IMU_WIRE_FLAG_SEQUENTIAL;
            completed->window_end_us=end;
            imu_transport_pending_add(1);
            imu_send_block(completed);
            imu_transport_pending_add(-1);
            continue;
        }
        /* STOP waits for a slot: never discard its partial block to acknowledge early. */
        configASSERT(xQueueReceive(s_free[stream], &replacement, portMAX_DELAY) == pdTRUE);
        completed->flags |= IMU_WIRE_FLAG_PARTIAL_WINDOW;
        completed->window_end_us = end;
        imu_transport_pending_add(1);
        configASSERT(xQueueSend(s_ready, &completed, portMAX_DELAY) == pdTRUE);
        s_active[stream] = replacement;
    }
    s_window_seq++;
    imu_transport_window(s_window_seq);
    if (write_reg(0x7e, 0xb0) != ESP_OK) fault = true;
    imu_transport_capture_state(fault ? 3 : 0);
    return fault;
}

static uint8_t apply_config(const imu_config_t *config);

static void acquisition_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    unsigned consecutive_errors = 0;
    bool acquiring = false, fault = false;
    uint32_t capture_id=UINT32_MAX;
    imu_transport_capture_state(0);
    while (true) {
        imu_config_t proposed; uint32_t revision;
        if(!acquiring && imu_transport_take_config(&proposed,&revision)) {
            uint8_t error=apply_config(&proposed);
            imu_transport_config_complete(&s_runtime,revision,error);
            if(error==4)fault=true;
        }
        bool requested = imu_transport_capture_requested();
        if (acquiring && !requested) {
            fault = stop_capture(false);
            acquiring = false;
            consecutive_errors = 0;
        }
        if (!acquiring && requested && !fault) {
            uint32_t requested_id=imu_transport_capture_id();
            if(requested_id!=capture_id) { s_collected[0]=s_collected[1]=0;capture_id=requested_id; }
            s_window_start = esp_timer_get_time();
            for (int i = 0; i < 2; i++) start_block(s_active[i], (imu_stream_id_t)i);
            if (fifo_start() != ESP_OK) {
                fault = true;
                imu_transport_capture_state(3);
            } else {
                acquiring = true;
                imu_transport_capture_state(1);
                ESP_LOGI(TAG, "START: adquisicion iniciada");
            }
        }
        if (!acquiring) {
            vTaskDelay(pdMS_TO_TICKS(20));
            wake = xTaskGetTickCount();
            continue;
        }
        rotate_windows(esp_timer_get_time());
        esp_err_t err = drain_fifo();
        if (err != ESP_OK) {
            for (int i = 0; i < 2; i++) s_active[i]->i2c_errors++;
            if (++consecutive_errors == 3) {
                ESP_LOGE(TAG, "Adquisicion detenida tras 3 errores I2C; revisar sensor");
                fault = true;
                stop_capture(true);
                acquiring = false;
            }
        } else consecutive_errors = 0;
        if(s_runtime.accel_samples && s_runtime.gyro_samples &&
            s_collected[0]>=s_runtime.accel_samples && s_collected[1]>=s_runtime.gyro_samples)
            imu_transport_finish_capture();
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(FIFO_POLL_MS));
    }
}

/* Codec helpers for the future connection layer. Console is a test sink.
 * imu_send_block receives an immutable block valid throughout the call. */
uint16_t imu_output_fragment_count(const imu_block_t *b)
{
    return s_runtime.json?imu_json_fragment_count(b):imu_wire_fragment_count(b);
}
size_t imu_output_encode_fragment(const imu_block_t *b,uint16_t index,uint8_t *out,size_t capacity)
{
    return s_runtime.json?imu_json_encode_fragment(b,s_device_id,s_boot_id,index,(char *)out,capacity):
        imu_wire_encode_fragment(b,s_device_id,s_boot_id,index,out,capacity);
}

void imu_output_identity(uint64_t *device, uint64_t *boot)
{
    if (device) *device = s_device_id;
    if (boot) *boot = s_boot_id;
}

esp_err_t imu_console_send_block(const imu_block_t *b)
{
#ifdef CONFIG_TIC3_WIRE_CONSOLE
    uint8_t packet[IMU_WIRE_PACKET_MAX];
    for (uint16_t part = 0; part < imu_output_fragment_count(b); part++) {
        size_t n = imu_output_encode_fragment(b, part, packet, sizeof(packet));
        uint8_t prefix[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16),
                              (uint8_t)(n >> 8), (uint8_t)n};
        if (!n || fwrite(prefix, 1, sizeof(prefix), stdout) != sizeof(prefix) ||
            fwrite(packet, 1, n, stdout) != n || fflush(stdout) != 0) {
            clearerr(stdout);
            return ESP_FAIL;
        }
    }
#else
    const char *name = b->stream == IMU_ACCEL ? "acceleration" : "angular_velocity";
#ifdef CONFIG_TIC3_PRINT_ARRAYS
    printf("{\"v\":1,\"stream\":\"%s\",\"window_seq\":%" PRIu32
           ",\"sample_hz\":%u,\"range\":%u,\"window_start_us\":%" PRId64
           ",\"window_end_us\":%" PRId64 ",\"interval_ms\":%" PRIu32
           ",\"sample_count\":%u,\"value_count\":%u"
           ",\"axes\":[\"x\",\"y\",\"z\"],\"unit\":\"raw_int16\""
           ",\"si_unit\":\"%s\",\"scale_to_si\":%.12g"
           ",\"timestamp_kind\":\"fifo_host_read\",\"buffer_dropped\":%" PRIu32
           ",\"fifo_skipped_frames\":%" PRIu32 ",\"fifo_errors\":%" PRIu32
           ",\"i2c_errors\":%" PRIu32 ",\"output_dropped_blocks\":%" PRIu32
           ",\"values\":[", name, b->window_seq, b->sample_hz, b->range,
           b->window_start_us, b->window_end_us, b->interval_ms, b->count, 3u * b->count,
           b->stream == IMU_ACCEL ? "m/s2" : "rad/s",
           (double)imu_raw_to_si(b->stream, b->range, 1), b->buffer_dropped,
           b->fifo_skipped_frames, b->fifo_errors, b->i2c_errors, b->output_dropped_blocks);
    for (size_t i = 0; i < b->count; i++) {
        printf("%s%d,%d,%d", i ? "," : "", b->xyz[i][0], b->xyz[i][1], b->xyz[i][2]);
    }
    printf("],\"read_us\":[");
    for (size_t i = 0; i < b->count; i++) printf("%s%" PRIu64, i ? "," : "", imu_block_read_us(b, i));
    printf("],\"sample_seq\":[");
    for (size_t i = 0; i < b->count; i++) printf("%s%" PRIu32, i ? "," : "", b->sample_seq[i]);
    printf("]}\n");
    if (fflush(stdout) != 0 || ferror(stdout)) {
        clearerr(stdout);
        return ESP_FAIL;
    }
#else
    ESP_LOGI(TAG, "%s bloque=%" PRIu32 " fs=%u Hz intervalo=%" PRIu32 " ms muestras=%u valores=%u "
             "buffer_drop=%" PRIu32 " fifo_skip=%" PRIu32 " fifo_err=%" PRIu32
             " i2c_err=%" PRIu32 " output_drop=%" PRIu32,
             name, b->window_seq, b->sample_hz, b->interval_ms, b->count, 3u * b->count,
             b->buffer_dropped, b->fifo_skipped_frames, b->fifo_errors,
             b->i2c_errors, b->output_dropped_blocks);
#endif
#endif
    return ESP_OK;
}

static void output_task(void *arg)
{
    (void)arg;
    imu_block_t *block;
    while (true) {
        if (xQueueReceive(s_ready, &block, portMAX_DELAY) == pdTRUE) {
            esp_err_t err = imu_send_block(block);
            if (err != ESP_OK) ESP_LOGE(TAG, "Salida de bloque fallo: %s", esp_err_to_name(err));
            /* TCP callback retains this immutable block until ACK; console and
             * UDP return after their attempt. No other task may reuse it yet. */
            configASSERT(xQueueSend(s_free[block->stream], &block, 0) == pdTRUE);
            imu_transport_pending_add(-1);
        }
    }
}

static void free_pools(void)
{
    for (int stream = 0; stream < 2; stream++) {
        for (int i = 0; i < MAX_BLOCKS; i++) {
            free(s_storage[stream][i]);
            s_storage[stream][i] = NULL;
        }
    }
}

static bool allocate_pools(void)
{
    uint16_t capacities[2];
    if (!imu_capacity_for(ACC_ODR, s_runtime.interval_ms, &capacities[0]) ||
        !imu_capacity_for(GYR_ODR, s_runtime.interval_ms, &capacities[1])) {
        ESP_LOGE(TAG, "Frecuencia/intervalo no admitido; no se inicia adquisicion");
        return false;
    }
    size_t total = BLOCKS_PER_STREAM * (imu_block_storage_bytes(capacities[0]) +
                                       imu_block_storage_bytes(capacities[1]));
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t reserve = 64u * 1024u;
    if (total > IMU_POOL_BUDGET_BYTES || free_heap < total + reserve) {
        ESP_LOGE(TAG, "Config rechazada: pools=%u bytes presupuesto=%u heap=%u "
                 "reserva=%u. Reducir intervalo, ODR o cantidad de bloques.",
                 (unsigned)total, IMU_POOL_BUDGET_BYTES, (unsigned)free_heap,
                 (unsigned)reserve);
        return false;
    }
    for (int stream = 0; stream < 2; stream++) {
        for (int i = 0; i < BLOCKS_PER_STREAM; i++) {
            size_t bytes = imu_block_storage_bytes(capacities[stream]);
            s_storage[stream][i] = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!imu_block_bind(&s_blocks[stream][i], s_storage[stream][i], bytes,
                                 capacities[stream])) {
                ESP_LOGE(TAG, "RAM contigua insuficiente; no se inicia adquisicion");
                free_pools();
                return false;
            }
        }
    }
    ESP_LOGI(TAG, "Intervalo=%d ms cap_acc=%u cap_gyro=%u bloques/flujo=%d RAM=%u bytes",
             s_runtime.interval_ms, capacities[0], capacities[1],
             BLOCKS_PER_STREAM, (unsigned)total);
    return true;
}

static uint8_t apply_config(const imu_config_t *c)
{
    if(!imu_config_valid(c))return 1;
    uint16_t cap[2];imu_capacity_for(c->accel_hz,c->interval_ms,&cap[0]);imu_capacity_for(c->gyro_hz,c->interval_ms,&cap[1]);
    size_t total=c->blocks*(imu_block_storage_bytes(cap[0])+imu_block_storage_bytes(cap[1]));
    if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<total+64u*1024u)return 3;
    void *replacement[2][MAX_BLOCKS]={{0}};
    for(int stream=0;stream<2;stream++)for(int i=0;i<c->blocks;i++) {
        replacement[stream][i]=heap_caps_malloc(imu_block_storage_bytes(cap[stream]),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(!replacement[stream][i])goto memory_failure;
    }
    if(bmi270_sensor_configure(c)!=ESP_OK || !bmi270_sensor_verify(c)) {
        bool recovered=bmi270_sensor_configure(&s_runtime)==ESP_OK && bmi270_sensor_verify(&s_runtime);
        for(int stream=0;stream<2;stream++)for(int i=0;i<MAX_BLOCKS;i++)free(replacement[stream][i]);
        if(!recovered)imu_transport_capture_state(3);
        return recovered?5:4;
    }
    free_pools();s_runtime=*c;xQueueReset(s_ready);
    for(int stream=0;stream<2;stream++) {
        xQueueReset(s_free[stream]);
        for(int i=0;i<c->blocks;i++) {
            s_storage[stream][i]=replacement[stream][i];
            configASSERT(imu_block_bind(&s_blocks[stream][i],s_storage[stream][i],imu_block_storage_bytes(cap[stream]),cap[stream]));
            if(i){imu_block_t *block=&s_blocks[stream][i];configASSERT(xQueueSend(s_free[stream],&block,0)==pdTRUE);}
        }
        s_active[stream]=&s_blocks[stream][0];start_block(s_active[stream],(imu_stream_id_t)stream);
    }
    ESP_LOGI(TAG,"CONFIG aplicada: acc=%u gyro=%u intervalo=%lu modo=%s red=%s",c->accel_hz,c->gyro_hz,(unsigned long)c->interval_ms,c->parallel?"paralelo":"secuencial",c->udp?"UDP":"TCP");
    return 0;
memory_failure:
    for(int stream=0;stream<2;stream++)for(int i=0;i<MAX_BLOCKS;i++)free(replacement[stream][i]);
    return 3;
}

void imu_acquisition_start(void)
{
    imu_config_defaults(&s_runtime);
    if (!rate_supported(ACC_ODR) || !rate_supported(GYR_ODR)) {
        ESP_LOGE(TAG, "ODR admitidas: 25, 50, 100, 200 o 400 Hz");
        return;
    }
    /* Initialize WiFi first, so pool validation accounts for its actual heap. */
    esp_err_t wifi_err = imu_wifi_start();
    if (wifi_err != ESP_OK)
        ESP_LOGW(TAG, "WiFi no iniciado (%s); adquisicion disponible por consola",
                 esp_err_to_name(wifi_err));
    if (!allocate_pools()) return;
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));
    for (int i = 0; i < 6; i++) s_device_id = (s_device_id << 8) | mac[i];
    s_boot_id = ((uint64_t)esp_random() << 32) | esp_random();
    ESP_ERROR_CHECK(imu_transport_init());
    ESP_ERROR_CHECK(bmi270_sensor_start(&s_runtime));
    s_ready = xQueueCreate(2 * (MAX_BLOCKS - 1), sizeof(imu_block_t *));
    if (!s_ready) {
        ESP_LOGE(TAG, "Sin RAM para cola de salida; adquisicion no iniciada");
        free_pools();
        return;
    }
    s_window_start = esp_timer_get_time();
    for (int stream = 0; stream < 2; stream++) {
        s_free[stream] = xQueueCreate(MAX_BLOCKS, sizeof(imu_block_t *));
        if (!s_free[stream]) {
            ESP_LOGE(TAG, "Sin RAM para cola de bloques; adquisicion no iniciada");
            for (int j = 0; j < stream; j++) vQueueDelete(s_free[j]);
            vQueueDelete(s_ready);
            free_pools();
            return;
        }
        s_active[stream] = &s_blocks[stream][0];
        start_block(s_active[stream], (imu_stream_id_t)stream);
        for (int i = 1; i < BLOCKS_PER_STREAM; i++) {
            imu_block_t *block = &s_blocks[stream][i];
            configASSERT(xQueueSend(s_free[stream], &block, 0) == pdTRUE);
        }
    }
    ESP_LOGI(TAG, "Arreglos independientes: acc=%d Hz gyro=%d Hz; ventana=%d ms; "
             "codec binario/JSON disponible; cliente socket segun configuracion",
             ACC_ODR, GYR_ODR, CONFIG_TIC3_INTERVAL_MS);
#if !defined(CONFIG_TIC3_PRINT_ARRAYS) && !defined(CONFIG_TIC3_WIRE_CONSOLE)
    ESP_LOGI(TAG, "Salida: resumen; habilitar TIC3_WIRE_CONSOLE para el protocolo");
#endif
    ESP_ERROR_CHECK(imu_transport_wait_ready());
    s_window_start = esp_timer_get_time();
    for (int stream = 0; stream < 2; stream++)
        start_block(s_active[stream], (imu_stream_id_t)stream);
    ESP_ERROR_CHECK(write_reg(0x49, 0x00));
    ESP_LOGI(TAG, "Listo; esperando START de la GUI");
    TaskHandle_t output_handle = NULL;
    bool output_ok = xTaskCreatePinnedToCore(output_task, "imu_output", 4096,
                                             NULL, 4, &output_handle, 0) == pdPASS;
    if (!output_ok || xTaskCreatePinnedToCore(acquisition_task, "imu_acquire", 6144,
                                              NULL, 8, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Sin RAM para tareas; adquisicion no iniciada");
        write_reg(0x49, 0x00);
        if (output_handle) vTaskDelete(output_handle);
        vQueueDelete(s_ready);
        for (int i = 0; i < 2; i++) vQueueDelete(s_free[i]);
        free_pools();
    }
}
