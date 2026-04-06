/**
 * ============================================================
 *  Monitor Industrial de Potencia y Temperatura v1.1
 *  Target: ESP32 (Arduino Framework)
 *  Autor: ESP32 Industrial Monitor — Portfolio Project
 * ============================================================
 *  Librerías requeridas (instalar desde Library Manager):
 *    - PubSubClient        (Nick O'Leary)     v2.8+
 *    - DHT sensor library  (Adafruit)         v1.4+
 *    - ArduinoJson         (Benoit Blanchon)  v6.x
 *    - OneWire             (Paul Stoffregen)  v2.3+
 *    - DallasTemperature   (Miles Burton)     v3.9+
 * ============================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "esp_task_wdt.h"   // WDT hardware del ESP32
#include "esp_system.h"     // esp_reset_reason()

// ============================================================
//  CONFIGURACIÓN — AJUSTAR ANTES DE FLASHEAR
// ============================================================
const char* WIFI_SSID   = "TU_SSID";
const char* WIFI_PASS   = "TU_PASSWORD";
const char* MQTT_BROKER = "broker.hivemq.com";
const int   MQTT_PORT   = 1883;
const char* DEVICE_ID   = "MON-001";

const char* TOPIC_TELEMETRIA = "industrial/monitor/telemetria";
const char* TOPIC_CMD        = "industrial/monitor/cmd";

// ============================================================
//  MAPA DE PINES (ESP32 DevKit v1)
// ============================================================
#define PIN_DHT          5    // DHT11 → DATA (GPIO5)
#define PIN_DS18B20      4    // DS18B20 → DATA (GPIO4)
#define PIN_RELE         26   // Módulo relé → IN  (HIGH = bobina activa)
#define PIN_LED_ROJO     27   // LED rojo → ÁNODO (100Ω a GND)
#define PIN_LED_VERDE    14   // LED verde → ÁNODO (10Ω a GND)

#define DHT_TYPE         DHT11

// ============================================================
//  PARÁMETROS DE SISTEMA
// ============================================================
#define WDT_TIMEOUT_S          5      // Watchdog: 5 s sin reset → reinicio
#define MA_WINDOW              10     // Ventana del filtro media móvil
#define TELEMETRIA_INTERVALO   1000   // ms — frecuencia de publicación MQTT
#define DHT_INTERVALO          2200   // ms — DHT11 máx ~0.5Hz
#define MQTT_RECONNECT_DELAY   5000   // ms — pausa entre intentos de reconexión

// Umbrales de protección térmica (con histéresis de 10 °C)
const float TEMP_TRIP_C  = 28.0f;  // Disparo: activa relé + LED rojo
const float TEMP_RESET_C = 25.0f;  // Reset: desactiva relé + LED verde

// ============================================================
//  OBJETOS GLOBALES
// ============================================================
DHT              dht(PIN_DHT, DHT_TYPE);
OneWire          oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);
WiFiClient       espClient;
PubSubClient     mqtt(espClient);

// Filtro media móvil — buffer circular (suaviza lecturas DS18B20)
float  ma_buf[MA_WINDOW] = {0.0f};
int    ma_idx  = 0;
float  ma_sum  = 0.0f;

// Estado del sistema
bool     proteccion_disparada = false;
bool     wdt_reiniciado       = false;
float    temp_disipador       = 25.0f;
float    temp_ambiente        = 25.0f;

// Timers (millis-based, no bloqueantes)
unsigned long t_telemetria    = 0;
unsigned long t_dht           = 0;
unsigned long t_mqtt_reconect = 0;

// ============================================================
//  MODELO MATEMÁTICO DE POTENCIA SIMULADA
//  Simula una fuente de alimentación 48V→12V @ 5A con degradación térmica
// ============================================================
struct DatosPotencia {
  float v_in;        // [V]
  float v_out;       // [V]
  float i_out;       // [A]
  float eficiencia;  // [%]
};

DatosPotencia simularPotencia(float temp_dis) {
  DatosPotencia d;

  d.v_in  = 48.0f + (float)random(-120, 120) / 200.0f;   // 48V ±0.6V
  d.v_out = 12.0f + (float)random(-60, 60)   / 300.0f;   // 12V ±0.2V
  d.i_out = 5.0f  + (float)random(-150, 150) / 150.0f;   // 5A  ±1A
  d.i_out = max(0.1f, d.i_out);

  // Modelo de eficiencia: η_base = 92%, penalización 0.25%/°C sobre 25°C
  float penalizacion = max(0.0f, (temp_dis - 25.0f) * 0.25f);
  float ruido_eta    = (float)random(-40, 40) / 100.0f;
  d.eficiencia = constrain(92.0f - penalizacion + ruido_eta, 60.0f, 99.5f);

  return d;
}

// ============================================================
//  FILTRO DE MEDIA MÓVIL (Moving Average)
//  Suaviza las lecturas del DS18B20 ante variaciones bruscas
// ============================================================
float movingAverage(float nueva_muestra) {
  ma_sum -= ma_buf[ma_idx];
  ma_buf[ma_idx] = nueva_muestra;
  ma_sum += nueva_muestra;
  ma_idx = (ma_idx + 1) % MA_WINDOW;
  return ma_sum / (float)MA_WINDOW;
}

// ============================================================
//  LECTURA SENSOR DS18B20 (digital 1-Wire)
// ============================================================
float leerTempDisipador() {
  ds18b20.requestTemperatures();
  float temp_raw = ds18b20.getTempCByIndex(0);

  // -127 = sensor no conectado o error
  if (temp_raw == DEVICE_DISCONNECTED_C || temp_raw < -55.0f) {
    Serial.println("[DS18B20] Error de lectura o sensor no conectado");
    return temp_disipador;  // Devolver último valor válido
  }

  return movingAverage(temp_raw);
}

// ============================================================
//  PROTECCIÓN TÉRMICA LOCAL (EDGE COMPUTING)
//  CRÍTICO: Opera sin internet. Independiente de WiFi/MQTT.
//  Histéresis: TRIP@28°C | RESET@25°C
// ============================================================
void gestionProteccionTermica(float temp) {
  if (!proteccion_disparada && temp >= TEMP_TRIP_C) {
    // ----- DISPARO -----
    proteccion_disparada = true;
    digitalWrite(PIN_RELE,     HIGH);
    digitalWrite(PIN_LED_ROJO, HIGH);
    digitalWrite(PIN_LED_VERDE, LOW);
    Serial.printf("[!!! ALARMA !!!] Protección térmica DISPARADA: %.1f°C >= %.0f°C\n",
                  temp, TEMP_TRIP_C);

  } else if (proteccion_disparada && temp <= TEMP_RESET_C) {
    // ----- RESET -----
    proteccion_disparada = false;
    digitalWrite(PIN_RELE,      LOW);
    digitalWrite(PIN_LED_ROJO,  LOW);
    digitalWrite(PIN_LED_VERDE, HIGH);
    Serial.printf("[OK] Protección térmica RESETEADA: %.1f°C <= %.0f°C\n",
                  temp, TEMP_RESET_C);
  }
}

// ============================================================
//  WIFI — Reconexión automática
// ============================================================
void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("[WiFi] Conectando");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 20) {
    delay(500);
    Serial.print(".");
    intentos++;
    esp_task_wdt_reset();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Conectado | IP: %s | RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("\n[WiFi] Timeout. Reintento en próximo ciclo.");
    WiFi.disconnect(true);
  }
}

// ============================================================
//  MQTT — Callback de comandos entrantes
// ============================================================
void callbackMQTT(char* topic, byte* payload, unsigned int length) {
  String cmd = "";
  for (unsigned int i = 0; i < length; i++) cmd += (char)payload[i];
  Serial.printf("[MQTT CMD] Topic: %s | Payload: %s\n", topic, cmd.c_str());

  // -------------------------------------------------------
  //  FORCE_WDT_TEST — Prueba del Watchdog
  //  Enviar: topic=industrial/monitor/cmd | payload=FORCE_WDT_TEST
  //  El sistema entrará en bucle infinito → WDT dispara en 5s
  // -------------------------------------------------------
  if (cmd == "FORCE_WDT_TEST") {
    Serial.println("╔══════════════════════════════════════╗");
    Serial.println("║  [WDT TEST] Bucle infinito iniciado  ║");
    Serial.println("║  Watchdog reiniciará en 5 segundos   ║");
    Serial.println("╚══════════════════════════════════════╝");
    while (true) { }  // WDT disparará en 5s
  }

  if (cmd == "RESET_PROTECCION") {
    proteccion_disparada = false;
    digitalWrite(PIN_RELE,      LOW);
    digitalWrite(PIN_LED_ROJO,  LOW);
    digitalWrite(PIN_LED_VERDE, HIGH);
    Serial.println("[MQTT] Protección reseteada manualmente.");
  }

  if (cmd == "STATUS") {
    Serial.printf("[STATUS] T_dis=%.1f°C | T_amb=%.1f°C | Prot=%s | WDT_reset=%s\n",
      temp_disipador, temp_ambiente,
      proteccion_disparada ? "ACTIVA" : "OK",
      wdt_reiniciado ? "SI" : "NO");
  }
}

// ============================================================
//  MQTT — Reconexión con back-off simple
// ============================================================
void conectarMQTT() {
  if (mqtt.connected()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long ahora = millis();
  if (ahora - t_mqtt_reconect < MQTT_RECONNECT_DELAY) return;
  t_mqtt_reconect = ahora;

  char clientId[32];
  snprintf(clientId, sizeof(clientId), "%s-%04X", DEVICE_ID, (uint16_t)esp_random());

  Serial.printf("[MQTT] Conectando como '%s'...\n", clientId);

  if (mqtt.connect(clientId)) {
    mqtt.subscribe(TOPIC_CMD, 1);
    Serial.printf("[MQTT] OK | Broker: %s:%d\n", MQTT_BROKER, MQTT_PORT);
  } else {
    Serial.printf("[MQTT] Fallo | rc=%d | Reintento en %ds\n",
                  mqtt.state(), MQTT_RECONNECT_DELAY / 1000);
  }
}

// ============================================================
//  TELEMETRÍA — Serializa y publica JSON por MQTT
// ============================================================
void publicarTelemetria() {
  StaticJsonDocument<512> doc;
  DatosPotencia pot = simularPotencia(temp_disipador);

  JsonObject meta = doc.createNestedObject("meta");
  meta["id_dispositivo"]      = DEVICE_ID;
  meta["tiempo_encendido_s"]  = (uint32_t)(millis() / 1000UL);
  meta["watchdog_reiniciado"] = wdt_reiniciado;

  JsonObject potencia = doc.createNestedObject("potencia");
  potencia["voltaje_entrada"]          = round(pot.v_in   * 100.0f) / 100.0f;
  potencia["voltaje_salida"]           = round(pot.v_out  * 100.0f) / 100.0f;
  potencia["corriente_salida"]         = round(pot.i_out  * 100.0f) / 100.0f;
  potencia["eficiencia_calculada_pct"] = round(pot.eficiencia * 10.0f) / 10.0f;

  JsonObject termico = doc.createNestedObject("termico");
  termico["temp_disipador_c"] = round(temp_disipador * 10.0f) / 10.0f;
  termico["temp_ambiente_c"]  = round(temp_ambiente  * 10.0f) / 10.0f;

  JsonObject estado = doc.createNestedObject("estado");
  estado["rele_activo"]                  = proteccion_disparada;
  estado["proteccion_termica_disparada"] = proteccion_disparada;

  char buffer[512];
  size_t len = serializeJson(doc, buffer);

  if (mqtt.publish(TOPIC_TELEMETRIA, buffer, len)) {
    Serial.printf("[TEL] T_dis=%.1f°C | Ef=%.1f%% | V_in=%.2fV | I=%.2fA | Rele=%s\n",
      temp_disipador, pot.eficiencia, pot.v_in, pot.i_out,
      proteccion_disparada ? "ON" : "OFF");
  } else {
    Serial.println("[TEL] ERROR: Fallo al publicar");
  }
}

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n");
  Serial.println("╔══════════════════════════════════════════════╗");
  Serial.println("║  Monitor Industrial Potencia & Temperatura   ║");
  Serial.println("╚══════════════════════════════════════════════╝");

  // --- Detectar causa del último reset ---
  esp_reset_reason_t motivo = esp_reset_reason();
  Serial.printf("[BOOT] Causa de reset: %d", motivo);
  if (motivo == ESP_RST_TASK_WDT || motivo == ESP_RST_WDT) {
    wdt_reiniciado = true;
    Serial.println(" → ** WATCHDOG DISPARO ** ← ALERTA REPORTADA EN TELEMETRÍA");
  } else if (motivo == ESP_RST_POWERON) {
    Serial.println(" → Power-on normal");
  } else if (motivo == ESP_RST_SW) {
    Serial.println(" → Reset por software");
  } else {
    Serial.println();
  }

  // --- Inicializar GPIO ---
  pinMode(PIN_RELE,      OUTPUT);
  pinMode(PIN_LED_ROJO,  OUTPUT);
  pinMode(PIN_LED_VERDE, OUTPUT);
  digitalWrite(PIN_RELE,      LOW);
  digitalWrite(PIN_LED_ROJO,  LOW);
  digitalWrite(PIN_LED_VERDE, HIGH);  // Verde = sistema OK

  // --- Inicializar DS18B20 ---
  ds18b20.begin();
  Serial.printf("[DS18B20] Sensores encontrados: %d\n", ds18b20.getDeviceCount());

  // --- Inicializar DHT11 ---
  dht.begin();
  delay(100);

  // --- Inicializar random seed ---
  randomSeed(esp_random());

  // --- Configurar Hardware Watchdog ---
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
  Serial.printf("[WDT] Configurado: timeout=%ds | panic=true\n", WDT_TIMEOUT_S);

  // --- Configurar cliente MQTT ---
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(callbackMQTT);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(60);

  // --- Conectar WiFi y MQTT ---
  conectarWiFi();
  conectarMQTT();

  Serial.println("[SETUP] Listo. Iniciando loop industrial...\n");
}

// ============================================================
//  LOOP PRINCIPAL
// ============================================================
void loop() {
  unsigned long ahora = millis();

  // ─── 1. ALIMENTAR WATCHDOG ───────────────────────────────
  esp_task_wdt_reset();

  // ─── 2. GESTIONAR CONECTIVIDAD ───────────────────────────
  conectarWiFi();
  conectarMQTT();
  mqtt.loop();

  // ─── 3. LEER TEMPERATURA DISIPADOR (DS18B20) ─────────────
  temp_disipador = leerTempDisipador();

  // ─── 4. PROTECCIÓN TÉRMICA LOCAL (CRÍTICO - SIEMPRE) ─────
  gestionProteccionTermica(temp_disipador);

  // ─── 5. LEER DHT11 (cada 2.2s) ───────────────────────────
  if (ahora - t_dht >= DHT_INTERVALO) {
    float t = dht.readTemperature();
    if (!isnan(t) && t > -40.0f && t < 85.0f) {
      temp_ambiente = t;
    } else {
      Serial.println("[DHT] Lectura inválida o sensor no conectado");
    }
    t_dht = ahora;
  }

  // ─── 6. PUBLICAR TELEMETRÍA (cada 1s) ────────────────────
  if (ahora - t_telemetria >= TELEMETRIA_INTERVALO) {
    if (mqtt.connected()) {
      publicarTelemetria();
    } else {
      Serial.println("[TEL] Skipped — MQTT desconectado");
    }
    t_telemetria = ahora;
  }
}
