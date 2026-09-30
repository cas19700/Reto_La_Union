/*
  NODO A - Panel de supervision de la bodega de azucar
  Hardware: ESP32 + DHT22 (temperatura) + LED de alarma

  Comunicacion:
    - ESP-NOW: enlace directo y cifrado con el Nodo B (no depende de la plataforma)
    - MQTT:    comunicacion con la plataforma de usuario (Node-RED)

  Patrones del LED:
    apagado          -> todo normal
    parpadeo rapido  -> alarma (temperatura alta en A o humedad alta en B)
    parpadeo lento   -> falla (sensor de A, sensor de B o enlace con B caido)

  Credenciales: secretos.h (no se sube a Git, ver secretos_ejemplo.h)

  Librerias: PubSubClient, DHT sensor library + Adafruit Unified Sensor, ArduinoJson 7
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "protocolo.h"

// Credenciales y configuracion de la red local. secretos.h NO se sube
// al repositorio (esta en .gitignore). Se crea copiando secretos_ejemplo.h
#if __has_include("secretos.h")
#include "secretos.h"
#else
#error "Falta secretos.h: copia secretos_ejemplo.h como secretos.h y llena tus valores"
#endif

// Las claves de ESP-NOW deben tener exactamente 16 caracteres (+ el '\0' final).
// Si alguien escribe una clave mas corta o mas larga el codigo no compila.
static_assert(sizeof(ESPNOW_PMK) == 17, "ESPNOW_PMK debe tener exactamente 16 caracteres");
static_assert(sizeof(ESPNOW_LMK) == 17, "ESPNOW_LMK debe tener exactamente 16 caracteres");

// ================= BROKER =================
const uint16_t MQTT_PORT   = 1883;
const char* MQTT_CLIENT_ID = "nodoA-esp32";
const char* MQTT_USER      = "nodoA";            // usuario definido en Mosquitto
const char* MQTT_PASS      = MQTT_PASS_NODO_A;   // viene de secretos.h

// ================= TOPICOS MQTT =================
const char* T_ESTADO    = "nodoA/estado";    // estado real del nodo (retenido)
const char* T_ACK       = "nodoA/ack";       // confirmacion de cada comando
const char* T_CONEXION  = "nodoA/conexion";  // online / offline (Last Will)
const char* T_CMD       = "nodoA/cmd";       // comandos solo para A
const char* T_CMD_TODOS = "todos/cmd";       // comandos para ambos nodos

// ================= PINES =================
const uint8_t PIN_DHT = 4;
const uint8_t PIN_LED = 18;   // LED con resistencia de 220 ohm a GND

// ================= CONSTANTES =================
const uint32_t PERIODO_RAPIDO_MS         = 250;
const uint32_t PERIODO_LENTO_MS          = 1000;
const uint32_t REINTENTO_MQTT_MS         = 5000;
const uint32_t MANUAL_SIN_SUPERVISION_MS = 30000;  // manual sin plataforma -> auto
const uint8_t  FALLOS_SENSOR_MAX         = 3;      // lecturas fallidas seguidas

// ================= OBJETOS =================
DHT dht(PIN_DHT, DHT22);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;

// ================= VARIABLES DE OPERACION (remotas) =================
float    umbralTemp  = 30.0;   // C para activar la alarma
float    histeresis  = 1.0;    // C bajo el umbral para desactivarla
uint32_t intervaloMs = 5000;   // periodo de muestreo del sensor

// ================= ESTADO LOCAL =================
enum PatronLed : uint8_t { LED_OFF = 0, LED_ON = 1, LED_RAPIDO = 2, LED_LENTO = 3 };
PatronLed ledManual = LED_OFF;   // patron elegido por el usuario en modo manual
bool  modoAuto      = true;
bool  alarmaTemp    = false;
bool  sensorOk      = false;
uint8_t fallosSensor = 0;
float ultimaTemp    = NAN;
bool  ledFisico     = false;

// ================= ESTADO DEL ENLACE CON B =================
bool     enlaceB      = false;
bool     alarmaHumB   = false;
bool     sensorBOk    = false;
float    humB         = NAN;
uint8_t  extractorB   = 0;
uint32_t tUltimoRxB   = 0;
uint32_t seqEsperadoB = 0;
bool     primerRxB    = true;
uint32_t perdidosB    = 0;
uint32_t seqTx        = 0;

// El callback de ESP-NOW corre en otra tarea: solo copia el mensaje
// y el loop lo procesa despues (misma idea que una ISR corta)
portMUX_TYPE muxRx = portMUX_INITIALIZER_UNLOCKED;
MensajeNodo rxBuf;
volatile bool rxNuevo = false;

// ================= TIEMPOS Y COMANDOS =================
uint32_t tUltimaLectura     = 0;
uint32_t tUltimoHeartbeat   = 0;
uint32_t tUltimoParpadeo    = 0;
uint32_t tUltimoIntentoMqtt = 0;
uint32_t tInicioSinMqtt     = 0;
bool     sinMqtt            = true;

char        ultimoIdCmd[32] = "";   // para no ejecutar dos veces el mismo comando
bool        ultimoOk        = false;
const char* ultimoError     = nullptr;

// =====================================================================
// PERSISTENCIA DE VARIABLES
// =====================================================================
void cargarConfig() {
  prefs.begin("nodoA", true);
  umbralTemp  = prefs.getFloat("umbral", 30.0);
  histeresis  = prefs.getFloat("hist", 1.0);
  intervaloMs = prefs.getUInt("intervalo", 5000);
  prefs.end();
}

void guardarConfig() {
  prefs.begin("nodoA", false);
  prefs.putFloat("umbral", umbralTemp);
  prefs.putFloat("hist", histeresis);
  prefs.putUInt("intervalo", intervaloMs);
  prefs.end();
}

// =====================================================================
// SENSOR Y ALARMA
// =====================================================================
void evaluarAlarma() {
  if (!sensorOk) { alarmaTemp = false; return; }
  if (!alarmaTemp && ultimaTemp >= umbralTemp) alarmaTemp = true;
  else if (alarmaTemp && ultimaTemp <= umbralTemp - histeresis) alarmaTemp = false;
}

void leerSensor() {
  float t = dht.readTemperature();
  if (isnan(t)) {
    // Una lectura fallida aislada es normal en el DHT22: solo se declara
    // falla despues de varias seguidas
    if (fallosSensor < 255) fallosSensor++;
    if (fallosSensor >= FALLOS_SENSOR_MAX) sensorOk = false;
    Serial.printf("Lectura DHT22 fallida (%u seguidas)\n", fallosSensor);
  } else {
    fallosSensor = 0;
    sensorOk = true;
    ultimaTemp = t;
  }
  evaluarAlarma();
}

// =====================================================================
// LED
// =====================================================================
// Logica automatica: la alarma tiene prioridad sobre la falla
PatronLed patronAuto() {
  if (alarmaTemp || (enlaceB && alarmaHumB)) return LED_RAPIDO;
  if (!sensorOk || !enlaceB || !sensorBOk)   return LED_LENTO;
  return LED_OFF;
}

PatronLed patronActual() {
  return modoAuto ? patronAuto() : ledManual;
}

const char* textoLed(PatronLed p) {
  switch (p) {
    case LED_ON:     return "on";
    case LED_RAPIDO: return "rapido";
    case LED_LENTO:  return "lento";
    default:         return "off";
  }
}

void actualizarLed(uint32_t ahora) {
  PatronLed p = patronActual();
  if (p == LED_OFF) {
    ledFisico = false;
  } else if (p == LED_ON) {
    ledFisico = true;
  } else {
    uint32_t periodo = (p == LED_RAPIDO) ? PERIODO_RAPIDO_MS : PERIODO_LENTO_MS;
    if (ahora - tUltimoParpadeo >= periodo) {
      tUltimoParpadeo = ahora;
      ledFisico = !ledFisico;
    }
  }
  digitalWrite(PIN_LED, ledFisico ? HIGH : LOW);
}

// =====================================================================
// MQTT: PUBLICACIONES
// =====================================================================
void publicarEstado() {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  doc["nodo"] = "A";
  if (sensorOk) doc["temp"] = roundf(ultimaTemp * 10) / 10.0f;
  else          doc["temp"] = nullptr;
  doc["sensorOk"]    = sensorOk;
  doc["alarmaTemp"]  = alarmaTemp;
  doc["led"]         = textoLed(patronActual());
  doc["modo"]        = modoAuto ? "auto" : "manual";
  doc["umbralTemp"]  = umbralTemp;
  doc["histeresis"]  = histeresis;
  doc["intervaloMs"] = intervaloMs;
  doc["enlaceB"]     = enlaceB;       // estado del enlace ESP-NOW visto por A
  doc["alarmaHumB"]  = alarmaHumB;
  doc["perdidosB"]   = perdidosB;
  doc["rssi"]        = WiFi.RSSI();
  doc["uptimeS"]     = millis() / 1000;

  char buffer[448];
  size_t n = serializeJson(doc, buffer, sizeof(buffer));
  mqtt.publish(T_ESTADO, (const uint8_t*)buffer, n, true);  // retenido
}

void publicarAck(const char* id, bool ok, const char* error) {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  doc["id"]   = id;
  doc["nodo"] = "A";
  doc["ok"]   = ok;
  if (error) doc["error"] = error;

  char buffer[192];
  size_t n = serializeJson(doc, buffer, sizeof(buffer));
  mqtt.publish(T_ACK, (const uint8_t*)buffer, n, false);
}

// =====================================================================
// MQTT: COMANDOS
// Formato: {"id":"c42","accion":"...", ...}
//   led    -> "valor": on | off | blink        (pasa a modo manual)
//   modo   -> "valor": auto | manual
//   set    -> umbralTemp (-40 a 80), histeresis (0 a 10), intervaloMs (2000 a 60000)
//   reporte-> solo publica el estado
// Devuelve true si se ejecuto. Si falla deja el motivo en "error".
// =====================================================================
bool ejecutarComando(JsonDocument& doc, const char*& error) {
  const char* accion = doc["accion"] | "";
  const char* valor  = doc["valor"]  | "";

  if (strcmp(accion, "led") == 0) {
    PatronLed p;
    if      (strcmp(valor, "on") == 0)    p = LED_ON;
    else if (strcmp(valor, "off") == 0)   p = LED_OFF;
    else if (strcmp(valor, "blink") == 0) p = LED_RAPIDO;
    else { error = "valor de led invalido (on, off, blink)"; return false; }
    modoAuto  = false;   // un comando manual toma el control del actuador
    ledManual = p;
    return true;
  }

  if (strcmp(accion, "modo") == 0) {
    if (strcmp(valor, "auto") == 0) {
      modoAuto = true;
    } else if (strcmp(valor, "manual") == 0) {
      if (modoAuto) ledManual = patronAuto();  // conserva la salida actual
      modoAuto = false;
    } else {
      error = "valor de modo invalido (auto, manual)";
      return false;
    }
    return true;
  }

  if (strcmp(accion, "set") == 0) {
    // Se valida todo antes de aplicar: o cambian todas o ninguna
    float    nUmbral    = umbralTemp;
    float    nHist      = histeresis;
    uint32_t nIntervalo = intervaloMs;
    bool     hayCambio  = false;

    if (!doc["umbralTemp"].isNull()) {
      if (!doc["umbralTemp"].is<float>()) { error = "umbralTemp debe ser numerico"; return false; }
      nUmbral = doc["umbralTemp"];
      if (nUmbral < -40 || nUmbral > 80) { error = "umbralTemp fuera de rango (-40 a 80)"; return false; }
      hayCambio = true;
    }
    if (!doc["histeresis"].isNull()) {
      if (!doc["histeresis"].is<float>()) { error = "histeresis debe ser numerica"; return false; }
      nHist = doc["histeresis"];
      if (nHist < 0 || nHist > 10) { error = "histeresis fuera de rango (0 a 10)"; return false; }
      hayCambio = true;
    }
    if (!doc["intervaloMs"].isNull()) {
      if (!doc["intervaloMs"].is<uint32_t>()) { error = "intervaloMs debe ser entero positivo"; return false; }
      nIntervalo = doc["intervaloMs"];
      if (nIntervalo < 2000 || nIntervalo > 60000) { error = "intervaloMs fuera de rango (2000 a 60000)"; return false; }
      hayCambio = true;
    }
    if (!hayCambio) { error = "set sin variables validas"; return false; }

    umbralTemp  = nUmbral;
    histeresis  = nHist;
    intervaloMs = nIntervalo;
    guardarConfig();
    evaluarAlarma();
    return true;
  }

  if (strcmp(accion, "reporte") == 0) return true;

  error = "accion desconocida";
  return false;
}

void callbackMqtt(char* topic, byte* payload, unsigned int length) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, length)) {
    publicarAck("", false, "json invalido");
    return;
  }

  const char* id = doc["id"] | "";
  if (id[0] == '\0') {
    publicarAck("", false, "falta id");
    return;
  }

  // Comando repetido (reintento de la plataforma): no se ejecuta de nuevo
  if (strcmp(id, ultimoIdCmd) == 0) {
    publicarAck(id, ultimoOk, ultimoError);
    return;
  }

  const char* error = nullptr;
  bool ok = ejecutarComando(doc, error);

  strlcpy(ultimoIdCmd, id, sizeof(ultimoIdCmd));
  ultimoOk    = ok;
  ultimoError = error;

  publicarAck(id, ok, error);
  publicarEstado();   // la plataforma ve el estado REAL tras ejecutar
}

void reconectarMqtt() {
  if (millis() - tUltimoIntentoMqtt < REINTENTO_MQTT_MS) return;
  tUltimoIntentoMqtt = millis();

  feedLoopWDT();
  Serial.print("Conectando a MQTT... ");
  bool ok = mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS,
                         T_CONEXION, 1, true, "offline");
  if (!ok) {
    Serial.printf("fallo (rc=%d)\n", mqtt.state());
    return;
  }
  Serial.println("conectado");
  mqtt.publish(T_CONEXION, "online", true);
  mqtt.subscribe(T_CMD, 1);
  mqtt.subscribe(T_CMD_TODOS, 1);
  publicarEstado();
}

// =====================================================================
// ESP-NOW
// =====================================================================
#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
#else
void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
#endif
  if (len != (int)sizeof(MensajeNodo)) return;
  MensajeNodo m;
  memcpy(&m, data, sizeof(m));
  if (m.magic != PROTO_MAGIC || m.version != PROTO_VERSION || m.origen != 'B') return;

  portENTER_CRITICAL(&muxRx);
  rxBuf = m;
  rxNuevo = true;
  portEXIT_CRITICAL(&muxRx);
}

bool iniciarEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error iniciando ESP-NOW");
    return false;
  }
  esp_now_set_pmk((const uint8_t*)ESPNOW_PMK);
  esp_now_register_recv_cb(onEspNowRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, MAC_NODO_B, 6);
  peer.channel = 0;             // 0 = canal actual de la radio
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = true;
  memcpy(peer.lmk, ESPNOW_LMK, 16);

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Error agregando al Nodo B como peer");
    return false;
  }
  return true;
}

void enviarHeartbeat() {
  MensajeNodo m = {};
  m.magic    = PROTO_MAGIC;
  m.version  = PROTO_VERSION;
  m.origen   = 'A';
  m.seq      = seqTx++;
  m.valor    = sensorOk ? ultimaTemp : NAN;
  m.alarma   = alarmaTemp ? 1 : 0;
  m.sensorOk = sensorOk ? 1 : 0;
  m.actuador = (uint8_t)patronActual();
  esp_now_send(MAC_NODO_B, (const uint8_t*)&m, sizeof(m));
}

void procesarMensajeB(uint32_t ahora) {
  if (!rxNuevo) return;

  MensajeNodo m;
  portENTER_CRITICAL(&muxRx);
  m = rxBuf;
  rxNuevo = false;
  portEXIT_CRITICAL(&muxRx);

  // Si la secuencia salta hubo mensajes perdidos.
  // Si retrocede, B se reinicio y se toma como nuevo inicio.
  if (!primerRxB && m.seq > seqEsperadoB) perdidosB += m.seq - seqEsperadoB;
  primerRxB    = false;
  seqEsperadoB = m.seq + 1;

  bool cambio = !enlaceB || (alarmaHumB != (m.alarma == 1)) || (sensorBOk != (m.sensorOk == 1));

  tUltimoRxB = ahora;
  enlaceB    = true;
  humB       = m.valor;
  alarmaHumB = (m.alarma == 1);
  sensorBOk  = (m.sensorOk == 1);
  extractorB = m.actuador;

  if (cambio) publicarEstado();
}

void verificarEnlaceB(uint32_t ahora) {
  if (enlaceB && ahora - tUltimoRxB > TIMEOUT_ENLACE_MS) {
    enlaceB    = false;
    alarmaHumB = false;   // no se confia en un dato viejo
    Serial.println("Enlace con B perdido");
    publicarEstado();
  }
}

// =====================================================================
// COMPORTAMIENTO SEGURO SIN PLATAFORMA
// Si el nodo queda en manual y pierde la plataforma nadie puede
// supervisarlo: despues de 30 s vuelve solo a modo automatico
// =====================================================================
void gestionarModoSeguro(uint32_t ahora) {
  if (mqtt.connected()) {
    sinMqtt = false;
    return;
  }
  if (!sinMqtt) {
    sinMqtt = true;
    tInicioSinMqtt = ahora;
  }
  if (!modoAuto && ahora - tInicioSinMqtt > MANUAL_SIN_SUPERVISION_MS) {
    modoAuto = true;
    Serial.println("Sin plataforma: regreso a modo automatico");
  }
}

// =====================================================================
// SETUP Y LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  dht.begin();
  cargarConfig();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CANAL);
  Serial.print("MAC de este nodo: ");
  Serial.println(WiFi.macAddress());

  iniciarEspNow();

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(callbackMqtt);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(3);
  mqtt.setKeepAlive(5);   // el broker detecta la caida en ~7.5 s

  delay(2000);            // el DHT22 necesita ~2 s tras energizarse
  leerSensor();
  tUltimaLectura = millis();

  enableLoopWDT();        // si el loop se traba ~5 s el ESP32 se reinicia
}

void loop() {
  uint32_t ahora = millis();

  // 1. Plataforma (MQTT)
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) reconectarMqtt();
    else mqtt.loop();
  }

  // 2. Enlace directo con B (ESP-NOW)
  procesarMensajeB(ahora);
  verificarEnlaceB(ahora);

  // 3. Sensor
  if (ahora - tUltimaLectura >= intervaloMs) {
    tUltimaLectura = ahora;
    bool alarmaAntes = alarmaTemp;
    leerSensor();
    publicarEstado();
    if (alarmaTemp != alarmaAntes) {   // avisa a B de inmediato
      enviarHeartbeat();
      tUltimoHeartbeat = ahora;
    }
  }

  // 4. Heartbeat periodico hacia B
  if (ahora - tUltimoHeartbeat >= HEARTBEAT_MS) {
    tUltimoHeartbeat = ahora;
    enviarHeartbeat();
  }

  // 5. Seguridad y actuador
  gestionarModoSeguro(ahora);
  actualizarLed(ahora);
}
