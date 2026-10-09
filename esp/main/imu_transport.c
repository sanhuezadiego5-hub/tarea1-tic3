#include "sdkconfig.h"
#include "imu_transport.h"
#include "imu_output.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static void reset_completion(void);
static portMUX_TYPE s_config_lock = portMUX_INITIALIZER_UNLOCKED;
static imu_config_t s_config, s_requested_config;
static uint32_t s_config_revision, s_request_revision, s_report_revision;
static uint8_t s_report_error;
static bool s_config_pending, s_report_ready;
static uint8_t s_preview[32];
void imu_transport_get_config(imu_config_t *c) {
    portENTER_CRITICAL(&s_config_lock); *c = s_config; portEXIT_CRITICAL(&s_config_lock);
}
bool imu_transport_take_config(imu_config_t *c, uint32_t *revision) {
    portENTER_CRITICAL(&s_config_lock);
    bool pending = s_config_pending;
    if (pending) { *c=s_requested_config; *revision=s_request_revision; s_config_pending=false; }
    portEXIT_CRITICAL(&s_config_lock); return pending;
}
void imu_transport_config_complete(const imu_config_t *c, uint32_t revision, uint8_t error) {
    portENTER_CRITICAL(&s_config_lock);
    if (!error) {
        s_config=*c; s_config_revision=revision;
        uint8_t window[4]; memcpy(window,s_preview+8,4);memset(s_preview,0,sizeof(s_preview));memcpy(s_preview+8,window,4);
    }
    s_report_revision=revision; s_report_error=error; s_report_ready=true;
    portEXIT_CRITICAL(&s_config_lock);
    if (!error) reset_completion();
}
void imu_transport_window(uint32_t window) {
    portENTER_CRITICAL(&s_config_lock);
    for(int i=0;i<4;i++)s_preview[8+i]=(uint8_t)(window>>(24-8*i));
    portEXIT_CRITICAL(&s_config_lock);
}
void imu_transport_preview(unsigned stream, const int16_t xyz[3], uint32_t count, uint32_t read_ms) {
    if (stream>1) return;
    portENTER_CRITICAL(&s_config_lock);
    for (int i=0;i<4;i++) s_preview[stream*4+i]=(uint8_t)(count>>(24-8*i));
    for (int i=0;i<3;i++) { s_preview[16+stream*6+2*i]=(uint16_t)xyz[i]>>8; s_preview[17+stream*6+2*i]=xyz[i]; }
    for (int i=0;i<4;i++) s_preview[28+i]=(uint8_t)(read_ms>>(24-8*i));
    portEXIT_CRITICAL(&s_config_lock);
}


#ifndef CONFIG_TIC3_NETWORK_SEND
uint32_t imu_transport_capture_id(void) { return 0; }
bool imu_transport_capture_requested(void) { return true; }
void imu_transport_capture_state(uint8_t state) { (void)state; }
void imu_transport_pending_add(int delta) { (void)delta; }
static void reset_completion(void) {}
void imu_transport_finish_capture(void) {}
esp_err_t imu_transport_init(void) { imu_config_defaults(&s_config); return ESP_OK; }
esp_err_t imu_transport_wait_ready(void) { return ESP_OK; }
esp_err_t imu_send_block(const imu_block_t *block) { return imu_console_send_block(block); }
#else
#include <errno.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "imu_wifi.h"
#include "imu_wire.h"

static const char *TAG = "imu_transport";
static int s_tcp = -1, s_udp = -1;
static uint32_t s_generation;
static uint64_t s_device, s_boot;
static struct sockaddr_in s_destination;
static atomic_bool s_capture_requested;
static atomic_uint s_capture_state;
static atomic_int s_pending;
static atomic_bool s_finished;
static atomic_uint s_start_revision;
uint32_t imu_transport_capture_id(void) { return atomic_load(&s_start_revision); }
static uint32_t s_socket_revision;
static void reset_completion(void) { atomic_store(&s_finished,false); }
void imu_transport_finish_capture(void) { atomic_store(&s_capture_requested, false); atomic_store(&s_finished, true); }
static void control_task(void *arg);
bool imu_transport_capture_requested(void) {
    return atomic_load(&s_capture_requested) && !atomic_load(&s_finished) &&
           !(atomic_load(&s_capture_state) == 0 && atomic_load(&s_pending) > 0);
}
void imu_transport_capture_state(uint8_t state) { atomic_store(&s_capture_state, state); }
void imu_transport_pending_add(int delta) { atomic_fetch_add(&s_pending, delta); }


static int64_t now_us(void) { return esp_timer_get_time(); }
static int64_t deadline(void) { return now_us() + 2000000LL; }

static bool same_network(uint32_t generation)
{
    imu_wifi_status_t status;
    imu_wifi_get_status(&status);
    return status.ready && status.generation == generation;
}

static void close_sockets(void)
{
    if (s_tcp >= 0) { close(s_tcp); s_tcp = -1; }
    if (s_udp >= 0) { close(s_udp); s_udp = -1; }
}

static bool wait_socket(int fd, bool write_ready, int64_t until)
{
    while (now_us() < until) {
        int64_t remaining = until - now_us();
        if (remaining <= 0) return false;
        struct timeval timeout = {.tv_sec = remaining / 1000000,
                                  .tv_usec = remaining % 1000000};
        fd_set set;
        FD_ZERO(&set); FD_SET(fd, &set);
        int result = select(fd + 1, write_ready ? NULL : &set,
                             write_ready ? &set : NULL, NULL, &timeout);
        if (result > 0) return true;
        if (result == 0 || errno != EINTR) return false;
    }
    return false;
}

static bool send_all(const uint8_t *data, size_t length, int64_t until)
{
    size_t offset = 0;
    while (offset < length) {
        if (!same_network(s_generation) || !wait_socket(s_tcp, true, until)) return false;
        int n = send(s_tcp, data + offset, length - offset, 0);
        if (n > 0) offset += (size_t)n;
        else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) return false;
    }
    return true;
}

static bool receive_all(uint8_t *data, size_t length, int64_t until)
{
    size_t offset = 0;
    while (offset < length) {
        if (!same_network(s_generation) || !wait_socket(s_tcp, false, until)) return false;
        int n = recv(s_tcp, data + offset, length - offset, 0);
        if (n > 0) offset += (size_t)n;
        else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) return false;
    }
    return true;
}

static bool send_frame(const uint8_t *data, size_t length, int64_t until)
{
    uint8_t prefix[4] = {(uint8_t)(length >> 24), (uint8_t)(length >> 16),
                          (uint8_t)(length >> 8), (uint8_t)length};
    return send_all(prefix, sizeof(prefix), until) && send_all(data, length, until);
}

static bool receive_control(uint8_t kind, uint32_t seq, uint8_t stream)
{
    uint8_t prefix[4], data[IMU_WIRE_HEADER_BYTES];
    int64_t until = deadline();
    if (!receive_all(prefix, 4, until)) return false;
    uint32_t length = ((uint32_t)prefix[0] << 24) | ((uint32_t)prefix[1] << 16) |
                      ((uint32_t)prefix[2] << 8) | prefix[3];
    if (length != sizeof(data) || !receive_all(data, length, until)) return false;
    return imu_wire_control_matches(data, length, kind, s_device, s_boot, seq, stream);
}

static bool hello(void)
{
    uint8_t data[IMU_WIRE_HEADER_BYTES];
    size_t n = imu_wire_encode_control(IMU_WIRE_HELLO, s_device, s_boot, 0, 255, data, sizeof(data));
    return send_frame(data, n, deadline()) && receive_control(IMU_WIRE_HELLO_ACK, 0, 255);
}

static bool resolve_and_connect(const imu_wifi_status_t *status)
{
    close_sockets();
    memset(&s_destination, 0, sizeof(s_destination));
    s_destination.sin_family = AF_INET;
    s_destination.sin_port = htons(status->server_port);
    if (inet_pton(AF_INET, status->server_host, &s_destination.sin_addr) != 1) {
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
        struct addrinfo *result = NULL;
        if (getaddrinfo(status->server_host, NULL, &hints, &result) != 0 || !result) return false;
        s_destination.sin_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr;
        freeaddrinfo(result);
    }
    s_generation = status->generation;
    if (!same_network(s_generation)) return false;
    s_tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_tcp < 0 || fcntl(s_tcp, F_SETFL, O_NONBLOCK) < 0) return false;
    imu_config_t config; imu_transport_get_config(&config);
    struct sockaddr_in control = s_destination;
    if (config.udp) control.sin_port = htons(CONFIG_TIC3_CONTROL_PORT);
    int result = connect(s_tcp, (struct sockaddr *)&control, sizeof(control));
    if (result < 0) {
        if (errno != EINPROGRESS || !wait_socket(s_tcp, true, deadline())) return false;
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(s_tcp, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error) return false;
    }
    if (!hello()) return false;
    if (config.udp) {
        s_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_udp < 0 || fcntl(s_udp, F_SETFL, O_NONBLOCK) < 0) return false;
    }
    portENTER_CRITICAL(&s_config_lock); s_socket_revision=s_config_revision; portEXIT_CRITICAL(&s_config_lock);
    imu_wifi_report_server_result(s_generation, true);
    ESP_LOGI(TAG, "HELLO confirmado con Raspberry %s:%u", status->server_host, status->server_port);
    return true;
}

esp_err_t imu_transport_init(void)
{
    imu_config_defaults(&s_config);
    s_report_ready=true;
    imu_output_identity(&s_device, &s_boot);
    ESP_LOGI(TAG, "Cliente de datos activado; esperara WiFi y HELLO antes de enviar");
    if (xTaskCreatePinnedToCore(control_task, "imu_control", 4096, NULL, 3, NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t imu_transport_wait_ready(void)
{
    unsigned failures = 0;
    ESP_LOGI(TAG, "Esperando WiFi y servidor antes de comenzar adquisicion");
    while (true) {
        imu_wifi_status_t status;
        imu_wifi_get_status(&status);
        if (!status.ready) { vTaskDelay(pdMS_TO_TICKS(250)); continue; }
        if (resolve_and_connect(&status)) return ESP_OK;
        imu_wifi_report_server_result(status.generation, false);
        close_sockets();
        uint32_t delay = imu_wifi_backoff_ms(failures);
        if (failures < 5) failures++;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

esp_err_t imu_send_block(const imu_block_t *block)
{
    unsigned failures = 0;
    imu_config_t config; imu_transport_get_config(&config);
    uint32_t revision; portENTER_CRITICAL(&s_config_lock); revision=s_config_revision; portEXIT_CRITICAL(&s_config_lock);
    uint8_t packet[IMU_WIRE_PACKET_MAX];
    while (true) {
        imu_wifi_status_t status;
        imu_wifi_get_status(&status);
        if (!status.ready) {
            close_sockets();
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (s_tcp < 0 || s_generation != status.generation || s_socket_revision != revision) {
            if (!resolve_and_connect(&status)) {
                imu_wifi_report_server_result(status.generation, false);
                close_sockets();
                uint32_t delay = imu_wifi_backoff_ms(failures);
                if (failures < 5) failures++;
                ESP_LOGW(TAG, "Servidor/HELLO no disponible; reintento en %u ms", (unsigned)delay);
                vTaskDelay(pdMS_TO_TICKS(delay));
                continue;
            }
        }
        if (config.udp && !hello()) { close_sockets(); continue; }
        int64_t until = deadline();
        bool sent = true;
        for (uint16_t part = 0; part < imu_output_fragment_count(block); part++) {
            size_t n = imu_output_encode_fragment(block, part, packet, sizeof(packet));
            if (!n) return ESP_ERR_INVALID_SIZE;
            if (config.udp) {
            if (!same_network(s_generation) || !wait_socket(s_udp, true, until) ||
                sendto(s_udp, packet, n, 0, (struct sockaddr *)&s_destination,
                         sizeof(s_destination)) != (int)n) { sent = false; break; }
            } else if (!send_frame(packet, n, until)) { sent = false; break; }
        }
        if (config.udp) {
            if (!sent) { close_sockets(); return ESP_FAIL; }
        } else if (!sent || !receive_control(IMU_WIRE_ACK, block->window_seq, (uint8_t)block->stream)) {
            close_sockets();
            uint32_t delay = imu_wifi_backoff_ms(failures);
            if (failures < 5) failures++;
            ESP_LOGW(TAG, "Lote %u sin ACK; se conserva y reenvia", (unsigned)block->window_seq);
            vTaskDelay(pdMS_TO_TICKS(delay));
            continue;
        }
        ESP_LOGI(TAG, "Datos confirmados/enviados: flujo=%u ventana=%u XYZ=%u descartes=%u",
                 (unsigned)block->stream, (unsigned)block->window_seq, block->count,
                 (unsigned)block->output_dropped_blocks);
        return ESP_OK;
    }
}

/* Independent control socket: STOP can arrive while DATA waits for its ACK. */
static bool control_io(int fd, uint8_t *bytes, size_t size, bool sending)
{
    size_t done = 0;
    int64_t until = deadline();
    while (done < size) {
        if (!wait_socket(fd, sending, until)) return false;
        int n = sending ? send(fd, bytes + done, size - done, 0)
                        : recv(fd, bytes + done, size - done, 0);
        if (n > 0) done += (size_t)n;
        else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) return false;
    }
    return true;
}

static bool control_exchange(int fd, uint8_t type, uint32_t seq, uint8_t state,
                             const uint8_t *payload, size_t bytes,
                             uint8_t reply[IMU_WIRE_HEADER_BYTES+32], size_t *reply_size)
{
    uint8_t frame[4 + IMU_WIRE_HEADER_BYTES + 32] = {0};
    size_t n=imu_wire_encode_extended(type,s_device,s_boot,seq,state,payload,bytes,frame+4,sizeof(frame)-4);
    if (!n) return false;
    frame[3]=(uint8_t)n;
    if (!control_io(fd, frame, n+4, true) || !control_io(fd, frame, 4, false)) return false;
    if (frame[0] || frame[1] || frame[2] || frame[3]<IMU_WIRE_HEADER_BYTES || frame[3]>IMU_WIRE_HEADER_BYTES+32) return false;
    *reply_size=frame[3];
    return control_io(fd,reply,*reply_size,false);
}

static int control_connect(const imu_wifi_status_t *status)
{
    struct sockaddr_in address = {.sin_family = AF_INET,
                                 .sin_port = htons(CONFIG_TIC3_CONTROL_PORT)};
    if (inet_pton(AF_INET, status->server_host, &address.sin_addr) != 1) {
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *result = NULL;
        if (getaddrinfo(status->server_host, NULL, &hints, &result) != 0 || !result) return -1;
        address.sin_addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr;
        freeaddrinfo(result);
    }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { close(fd); return -1; }
    int result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result < 0) {
        int error = 0; socklen_t size = sizeof(error);
        if (errno != EINPROGRESS || !wait_socket(fd, true, deadline()) ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error) {
            close(fd); return -1;
        }
    }
    uint8_t reply[IMU_WIRE_HEADER_BYTES+32]; size_t reply_size;
    if (!control_exchange(fd, IMU_WIRE_HELLO, 0, 255, NULL, 0, reply, &reply_size) ||
        !imu_wire_control_matches(reply, reply_size, IMU_WIRE_HELLO_ACK, s_device, s_boot, 0, 255)) {
        close(fd); return -1;
    }
    return fd;
}

static void control_task(void *arg)
{
    (void)arg; int fd=-1;
    uint32_t generation=0,command=0;
    uint8_t reply[IMU_WIRE_HEADER_BYTES+32],payload[32];
    while (true) {
        imu_wifi_status_t status; imu_wifi_get_status(&status);
        if (!status.ready || (fd>=0 && generation!=status.generation)) {
            if(fd>=0) { close(fd); }
            fd=-1;atomic_store(&s_capture_requested,false);
        }
        if(!status.ready){vTaskDelay(pdMS_TO_TICKS(100));continue;}
        if(fd<0){fd=control_connect(&status);generation=status.generation;
            if(fd<0){vTaskDelay(pdMS_TO_TICKS(1000));continue;}
            portENTER_CRITICAL(&s_config_lock);
            if(!s_report_ready){s_report_ready=true;s_report_revision=command;s_report_error=0;}
            portEXIT_CRITICAL(&s_config_lock);
        }
        imu_config_t config; imu_transport_get_config(&config);
        uint8_t state=(uint8_t)atomic_load(&s_capture_state);
        if(state!=3 && (!atomic_load(&s_capture_requested)||atomic_load(&s_finished)||state==0) &&
            (state==1||atomic_load(&s_pending)>0))state=2;
        if(state==0){if(atomic_load(&s_finished))state=config.udp?7:6;else if(config.udp)state=4;}
        uint8_t kind=IMU_WIRE_STATE,request_state=state;
        uint32_t request_seq=command;bool report;
        portENTER_CRITICAL(&s_config_lock);
        report=s_report_ready;
        if(report){request_seq=s_report_revision;request_state=s_report_error?s_report_error:255;
            kind=s_report_error?IMU_WIRE_CONFIG_ERROR:IMU_WIRE_CONFIG_OK;
            imu_config_pack(&s_config,payload);
        }else memcpy(payload,s_preview,32);
        portEXIT_CRITICAL(&s_config_lock);
        size_t bytes=kind==IMU_WIRE_CONFIG_ERROR?0:32,reply_size=0;
        if(!control_exchange(fd,kind,request_seq,request_state,payload,bytes,reply,&reply_size)){
            close(fd);fd=-1;atomic_store(&s_capture_requested,false);vTaskDelay(pdMS_TO_TICKS(500));continue;}
        uint32_t seq=((uint32_t)reply[24]<<24)|((uint32_t)reply[25]<<16)|((uint32_t)reply[26]<<8)|reply[27];
        if(reply[5]==IMU_WIRE_CONFIG && imu_wire_extended_matches(reply,reply_size,IMU_WIRE_CONFIG,s_device,s_boot,seq,255)){
            imu_config_t proposed;
            if(atomic_load(&s_capture_state)!=0||atomic_load(&s_pending)>0){imu_transport_config_complete(&config,seq,2);}
            else if(!imu_config_unpack(reply+IMU_WIRE_HEADER_BYTES,reply_size-IMU_WIRE_HEADER_BYTES,&proposed)){
                imu_transport_config_complete(&config,seq,1);
            }else{
                portENTER_CRITICAL(&s_config_lock);
                if(seq!=s_request_revision){s_request_revision=seq;s_requested_config=proposed;s_config_pending=true;}
                portEXIT_CRITICAL(&s_config_lock);
            }
        }else{
            bool start=imu_wire_control_matches(reply,reply_size,IMU_WIRE_START,s_device,s_boot,seq,255);
            bool stop=imu_wire_control_matches(reply,reply_size,IMU_WIRE_STOP,s_device,s_boot,seq,255);
            if(start||stop){
                if(start&&seq!=command) { atomic_store(&s_finished,false); atomic_store(&s_start_revision,seq); }
                bool accept=start && !(seq==command && atomic_load(&s_finished));
                command=seq;atomic_store(&s_capture_requested,accept);
            }
            else if(imu_wire_control_matches(reply,reply_size,IMU_WIRE_CONTROL_ACK,s_device,s_boot,request_seq,request_state)){
                if(report){portENTER_CRITICAL(&s_config_lock);if(s_report_revision==request_seq)s_report_ready=false;portEXIT_CRITICAL(&s_config_lock);command=request_seq;}
            }else{close(fd);fd=-1;atomic_store(&s_capture_requested,false);}
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif
