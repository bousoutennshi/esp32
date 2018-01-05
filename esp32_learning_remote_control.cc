#include <WiFi.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt.h"
#include "http_parser.h"

const char* ssid = "xxxxxxxxxx";
const char* password = "xxxxxxxxxx";

#define RMT_TX_CHANNEL RMT_CHANNEL_4
#define RMT_TX_GPIO_NUM GPIO_NUM_33

#define RMT_RX_CHANNEL RMT_CHANNEL_0
#define RMT_RX_GPIO_NUM GPIO_NUM_25

#define RMT_CLK_DIV 100
#define RMT_TICK_10_US (80000000/RMT_CLK_DIV/100000)
#define rmt_item32_TIMEOUT_US 10000

#define MAX_SIGNAL_LEN 2048

void process();

WiFiServer server(80);

void setup() {
  Serial.begin(115200);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("");

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  server.begin();
  init_tx();
  init_rx();
}

void loop(){
  process();
}

/* ir */
bool ir_use = false;
size_t received = 0;
rmt_item32_t signals[MAX_SIGNAL_LEN];

void init_tx() {
  rmt_config_t rmt_tx;
  rmt_tx.rmt_mode = RMT_MODE_TX;
  rmt_tx.channel = RMT_TX_CHANNEL;
  rmt_tx.gpio_num = RMT_TX_GPIO_NUM;
  rmt_tx.mem_block_num = 4;
  rmt_tx.clk_div = RMT_CLK_DIV;
  rmt_tx.tx_config.loop_en = false;
  rmt_tx.tx_config.carrier_duty_percent = 50;
  rmt_tx.tx_config.carrier_freq_hz = 38000;
  rmt_tx.tx_config.carrier_level = RMT_CARRIER_LEVEL_HIGH;
  rmt_tx.tx_config.carrier_en = 1;
  rmt_tx.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
  rmt_tx.tx_config.idle_output_en = true;
  rmt_config(&rmt_tx);
  rmt_driver_install(rmt_tx.channel, 0, 0);
}

void init_rx() {
  rmt_config_t rmt_rx;
  rmt_rx.rmt_mode = RMT_MODE_RX;
  rmt_rx.channel = RMT_RX_CHANNEL;
  rmt_rx.clk_div = RMT_CLK_DIV;
  rmt_rx.gpio_num = RMT_RX_GPIO_NUM;
  rmt_rx.mem_block_num = 4;
  rmt_rx.rx_config.filter_en = true;
  rmt_rx.rx_config.filter_ticks_thresh = 100;
  rmt_rx.rx_config.idle_threshold = rmt_item32_TIMEOUT_US / 10 * (RMT_TICK_10_US);
  rmt_config(&rmt_rx);
  rmt_driver_install(rmt_rx.channel, 1000, 0);
}

void rmt_tx_task(void *) {
  Serial.println("send...");
  rmt_write_items(RMT_TX_CHANNEL, signals, received, true);
  rmt_wait_tx_done(RMT_TX_CHANNEL, portMAX_DELAY);

  Serial.println("send done");

  ir_use = false;
  vTaskDelete(NULL);
}

void rmt_rx_task(void *) {
  RingbufHandle_t rb = NULL;
  rmt_get_ringbuf_handle(RMT_RX_CHANNEL, &rb);
  rmt_rx_start(RMT_RX_CHANNEL, 1);

  size_t rx_size = 0;
  Serial.println("wait ir signal...");
  rmt_item32_t *item = (rmt_item32_t*)xRingbufferReceive(rb, &rx_size, 10000);
  rmt_rx_stop(RMT_RX_CHANNEL);
  if(!item) {
    Serial.println("no data received");
    ir_use = false;
    vTaskDelete(NULL);
    return;
  }
  Serial.print("received items: ");
  Serial.println(rx_size);

  memcpy(signals, item, sizeof(rmt_item32_t) * rx_size);
  for (int i = 0; i < rx_size; ++i) {
    signals[i].level0 = ~signals[i].level0;
    signals[i].level1 = ~signals[i].level1;
  }
  received = rx_size;
  vRingbufferReturnItem(rb, (void*)item);

  Serial.println("recv done");

  rmt_rx_stop(RMT_RX_CHANNEL);
  ir_use = false;
  vTaskDelete(NULL);
}

/* http */
void send_202(WiFiClient &client) {
  client.print("HTTP/1.1 202 Accepted\r\n");
  client.print("Connection: close\r\n");
  client.print("Content-Length: 0\r\n\r\n");
}

void send_400(WiFiClient &client) {
  client.print("HTTP/1.1 400 Bad Request\r\n");
  client.print("Connection: close\r\n");
  client.print("Content-Length: 0\r\n\r\n");
}

void send_404(WiFiClient &client) {
  client.print("HTTP/1.1 404 Not Found\r\n");
  client.print("Connection: close\r\n");
  client.print("Content-Length: 0\r\n\r\n");
}

void send_409(WiFiClient &client) {
  client.print("HTTP/1.1 409 Conflict\r\n");
  client.print("Connection: close\r\n");
  client.print("Content-Length: 0\r\n\r\n");
}

char url[128];
int on_url(http_parser *http_parser, const char *buf, size_t len) {
  if (sizeof(url) <= strlen(url) + len) {
    Serial.println("URL too long");
    return 1;
  }
  strncat(url, buf, len);

  return 0;
}

char body[sizeof(signals)];
size_t bodylen = 0;
int on_body(http_parser *http_parser, const char *buf, size_t len) {
  if (sizeof(body) < bodylen + len) {
    Serial.println("Body too long");
    return 1;
  }
  memcpy(body + bodylen, buf, len);
  bodylen += len;
  return 0;
}

bool request_end = false;
int on_message_complete(http_parser *http_parser) {
  request_end = true;
}

int on_chunk_complete(http_parser *http_parser) {
  request_end = true;
}

void process() {
  char buf[1024];
  http_parser parser;
  http_parser_settings settings;
  WiFiClient client = server.available();
  bool error = false;

  memset(url, 0, sizeof(url));
  request_end = false;
  bodylen = 0;
  http_parser_init(&parser, HTTP_REQUEST);
  http_parser_settings_init(&settings);
  settings.on_url = on_url;
  settings.on_body = on_body;
  settings.on_message_complete = on_message_complete;
  settings.on_chunk_complete = on_chunk_complete;

  if (!client) { return; }
  while (client.connected()) {
    if (client.available()) {
      size_t nread = client.readBytes(buf, sizeof(buf));
      size_t nparsed = http_parser_execute(&parser, &settings, buf, nread);
      if (nread != nparsed || parser.http_errno != HPE_OK) {
        error = true;
        break;
      }
      if (request_end) {
        break;
      }
    }
  }

  if (!request_end || error) {
    send_400(client);
  } else if (strcmp(url, "/dump") == 0) {
    client.print("HTTP/1.1 200 OK\r\nContent-Type: octet-stream\r\nContent-Length: ");
    client.print(sizeof(rmt_item32_t) * received);
    client.print("\r\n\r\n");
    client.write((uint8_t*)signals, sizeof(rmt_item32_t) * received);
  } else if (strcmp(url, "/send") == 0) {
    received = bodylen / sizeof(rmt_item32_t);
    memcpy(signals, body, bodylen);
    if (ir_use) {
      send_409(client);
    } else {
      ir_use = true;
      xTaskCreate(rmt_tx_task, "rmt_tx_task", 2048, NULL, 10, NULL);
      send_202(client);
    }
  } else if (strcmp(url, "/recv") == 0) {
    if (ir_use) {
      send_409(client);
    } else {
      ir_use = true;
      xTaskCreate(rmt_rx_task, "rmt_rx_task", 2048, NULL, 10, NULL);
      send_202(client);
    }
  } else {
    send_404(client);
  }

  client.flush();
  client.stop();
}
