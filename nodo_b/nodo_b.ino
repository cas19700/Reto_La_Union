/*
  NODO B - Zona de bodega de azucar
  Hardware: ESP32 + DHT22 (humedad) + extractor (ventilador 5 V con 2N2222A y PWM)

  Comunicacion:
    - ESP-NOW: enlace directo y cifrado con el Nodo A (no depende de la plataforma)
    - MQTT:    comunicacion con la plataforma de usuario (Node-RED)

  Velocidad del extractor en modo automatico (de mayor a menor prioridad):
    100 %  -> humedad alta en B (alarma de humedad)
     60 %  -> temperatura alta en A (recibida por ESP-NOW)
     40 %  -> preventivo: sin informacion confiable
              (sensor de B fallando, sensor de A fallando o enlace con A caido)
      0 %  -> todo normal

  Tareas:
    - loop():        sensor, comandos MQTT, enlace con A y logica del nodo
    - temporizador:  extractor (con impulso de arranque) y heartbeat hacia A
                     cada 50 ms (esp_timer). Siguen funcionando aunque
                     mqtt.connect() bloquee el loop ~3 s.

  Credenciales: secretos.h (no se sube a Git, ver secretos_ejemplo.h)

  Librerias: PubSubClient, DHT sensor library + Adafruit Unified Sensor, ArduinoJson 7
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_arduino_version.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "protocolo.h"

#if __has_include("secretos.h")
#include "secretos.h"
#else
#error "Falta secretos.h: copia secretos_ejemplo.h como secretos.h y llena tus valores"
#endif

static_assert(sizeof(ESPNOW_PMK) == 17, "ESPNOW_PMK debe tener exactamente 16 caracteres");
static_assert(sizeof(ESPNOW_LMK) == 17, "ESPNOW_LMK debe tener exactamente 16 caracteres");

// ================= BROKER =================
const uint16_t MQTT_PORT   = 1883;
const char* MQTT_CLIENT_ID = "nodoB-esp32";
const char* MQTT_USER      = "nodoB";            // usuario definido en Mosquitto
const char* MQTT_PASS      = MQTT_PASS_NODO_B;   // viene de secretos.h

// ================= TOPICOS MQTT =================
const char* T_ESTADO    = "nodoB/estado";    // estado real del nodo (retenido)
const char* T_ACK       = "nodoB/ack";       // confirmacion de cada comando
const char* T_CONEXION  = "nodoB/conexion";  // online / offline (Last Will)
const char* T_CMD       = "nodoB/cmd";       // comandos solo para B
const char* T_CMD_TODOS = "todos/cmd";       // comandos para ambos nodos

// ================= PINES =================
const uint8_t PIN_DHT       = 4;    // fila de 3V3: requiere acceso a esa fila
const uint8_t PIN_EXTRACTOR = 25;   // fila de VIN, sin funcion de arranque, acepta PWM

// ================= CONSTANTES =================
const uint32_t TICK_SALIDA_MS            = 50;     // periodo del temporizador de salidas
const uint32_t PWM_FREQ_HZ               = 100;    // ventilador de 2 cables: 25 kHz no sirve
const uint8_t  PWM_BITS                  = 8;      // duty de 0 a 255
const uint32_t PWM_MAX                   = (1UL << PWM_BITS) - 1;
const uint8_t  EXTRACTOR_MIN             = 40;     // minimo util medido (30 %) + margen
const uint8_t  VEL_HUMEDAD               = 100;
const uint8_t  VEL_TEMP_A                = 60;
const uint8_t  VEL_PREVENTIVA            = 40;
const uint32_t KICK_MS                   = 500;    // impulso al 100 % al arrancar
const uint32_t REINTENTO_MQTT_MS         = 5000;
const uint32_t MANUAL_SIN_SUPERVISION_MS = 30000;  // manual sin plataforma -> auto
const uint8_t  FALLOS_SENSOR_MAX         = 3;      // lecturas fallidas seguidas

static_assert(KICK_MS      % TICK_SALIDA_MS == 0, "KICK_MS no es multiplo del tick");
static_assert(HEARTBEAT_MS % TICK_SALIDA_MS == 0, "HEARTBEAT_MS no es multiplo del tick");
static_assert(VEL_PREVENTIVA >= EXTRACTOR_MIN && VEL_TEMP_A >= EXTRACTOR_MIN,
              "Las velocidades automaticas deben ser al menos EXTRACTOR_MIN");

#if ESP_ARDUINO_VERSION_MAJOR < 3
const uint8_t PWM_CANAL = 0;   // en el core 2.x el PWM se maneja por canal
#endif

// ================= OBJETOS =================
DHT dht(PIN_DHT, DHT22);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;

// ================= VARIABLES DE OPERACION (remotas) =================
float    umbralHum     = 70.0;   // % HR para activar la alarma
float    histeresisHum = 5.0;    // puntos bajo el umbral para desactivarla
uint32_t intervaloMs   = 5000;   // periodo de muestreo del sensor

// ================= ESTADO LOCAL =================
bool    modoAuto        = true;
uint8_t extractorManual = 0;     // % elegido por el usuario en modo manual
bool    alarmaHum       = false;
bool    sensorOk        = false;
uint8_t fallosSensor    = 0;
float   ultimaHum       = NAN;

// ================= ESTADO DEL ENLACE CON A =================
bool     enlaceA      = false;
bool     alarmaTempA  = false;
bool     sensorAOk    = false;
float    tempA        = NAN;
uint32_t tUltimoRxA   = 0;
uint32_t seqEsperadoA = 0;
bool     primerRxA    = true;
uint32_t perdidosA    = 0;

portMUX_TYPE muxRx = portMUX_INITIALIZER_UNLOCKED;
MensajeNodo rxBuf;
volatile bool rxNuevo = false;

// ================= SALIDAS DEL TEMPORIZADOR =================
// El loop decide la velocidad y los datos para A y los deja en "salida".
// El temporizador solo lee esa copia y la ejecuta.
struct EstadoSalida {
  uint8_t velocidad;     // % que debe tener el extractor (0 o 40-100)
  float   hum;           // humedad para A (NAN si el sensor falla)
  uint8_t alarma;        // 1 si B tiene alarma de humedad
  uint8_t sensorOk;      // 1 si el sensor de B funciona
  bool    hbInmediato;   // pide un heartbeat sin esperar el periodo
};
portMUX_TYPE muxSalida = portMUX_INITIALIZER_UNLOCKED;
EstadoSalida salida = {VEL_PREVENTIVA, NAN, 0, 0, false};
esp_timer_handle_t timerSalida = nullptr;

// Estas solo las usa el temporizador (el loop no las toca)
uint32_t seqTx           = 0;
uint8_t  velAplicada     = 0;
uint32_t ticksKick       = 0;
uint32_t dutyEscrito     = 0xFFFFFFFF;   // fuerza la primera escritura
uint32_t ticksHeartbeat  = 0;

// ================= TIEMPOS Y COMANDOS =================
uint32_t tUltimaLectura     = 0;
uint32_t tUltimoIntentoMqtt = 0;
uint32_t tInicioSinMqtt     = 0;
bool     sinMqtt            = true;

char        ultimoIdCmd[32] = "";
bool        ultimoOk        = false;
const char* ultimoError     = nullptr;

// =====================================================================
// PWM DEL EXTRACTOR: capa de compatibilidad core 2.x / 3.x
// El core 3.x cambio la API del LEDC: usa el pin en lugar de un canal
// =====================================================================
bool pwmIniciar() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  return ledcAttach(PIN_EXTRACTOR, PWM_FREQ_HZ, PWM_BITS);
#else
  if (ledcSetup(PWM_CANAL, PWM_FREQ_HZ, PWM_BITS) == 0) return false;
  ledcAttachPin(PIN_EXTRACTOR, PWM_CANAL);
  return true;
#endif
}

void pwmEscribir(uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_EXTRACTOR, duty);
#else
  ledcWrite(PWM_CANAL, duty);
#endif
}

uint32_t dutyDePorcentaje(uint8_t pct) {
  return ((uint32_t)pct * PWM_MAX + 50) / 100;
}

// =====================================================================
// MOTIVO DEL ULTIMO REINICIO
// El ESP32 guarda en un registro por que se reinicio. Se imprime al
// arrancar y se publica en el estado para diagnosticar desde la plataforma.
// Nota: en el ESP32 original el boton EN cuenta como "encendido".
// =====================================================================
const char* motivoReinicio = "desconocido";

const char* textoReinicio(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "encendido";      // USB conectado o boton EN
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "falla";          // excepcion del programa
    case ESP_RST_INT_WDT:   return "watchdog_int";   // interrupcion trabada
    case ESP_RST_TASK_WDT:  return "watchdog";       // loop trabado (enableLoopWDT)
    case ESP_RST_WDT:       return "watchdog_otro";
    case ESP_RST_BROWNOUT:  return "brownout";       // caida de voltaje
    case ESP_RST_EXT:       return "externo";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    default:                return "desconocido";
  }
}

// =====================================================================
// PERSISTENCIA DE VARIABLES
// =====================================================================
void cargarConfig() {
  prefs.begin("nodoB", true);
  umbralHum     = prefs.getFloat("umbral", 70.0);
  histeresisHum = prefs.getFloat("hist", 5.0);
  intervaloMs   = prefs.getUInt("intervalo", 5000);
  prefs.end();
}

void guardarConfig() {
  prefs.begin("nodoB", false);
  prefs.putFloat("umbral", umbralHum);
  prefs.putFloat("hist", histeresisHum);
  prefs.putUInt("intervalo", intervaloMs);
  prefs.end();
}

// =====================================================================
// SENSOR Y ALARMA
// =====================================================================
void evaluarAlarma() {
  if (!sensorOk) { alarmaHum = false; return; }
  if (!alarmaHum && ultimaHum >= umbralHum) alarmaHum = true;
  else if (alarmaHum && ultimaHum <= umbralHum - histeresisHum) alarmaHum = false;
}

void leerSensor() {
  uint32_t t0 = millis();
  float h = dht.readHumidity();
  uint32_t duracion = millis() - t0;
  if (isnan(h)) {
    if (fallosSensor < 255) fallosSensor++;
    if (fallosSensor >= FALLOS_SENSOR_MAX) sensorOk = false;
    Serial.printf("Lectura DHT22 fallida (%u seguidas) en %lu ms\n", fallosSensor, (unsigned long)duracion);
  } else {
    fallosSensor = 0;
    sensorOk = true;
    ultimaHum = h;
    Serial.printf("Humedad: %.1f %% (lectura en %lu ms)\n", h, (unsigned long)duracion);
  }
  evaluarAlarma();
}

// =====================================================================
// LOGICA DEL EXTRACTOR
// Misma idea que el LED de A: alarma antes que falla, falla antes que normal
// =====================================================================
uint8_t velocidadAuto() {
  if (alarmaHum)                          return VEL_HUMEDAD;
  if (enlaceA && sensorAOk && alarmaTempA) return VEL_TEMP_A;
  if (!sensorOk || !enlaceA || !sensorAOk) return VEL_PREVENTIVA;
  return 0;
}

uint8_t velocidadActual() {
  return modoAuto ? velocidadAuto() : extractorManual;
}

// Motivo de la velocidad actual: le dice a la plataforma POR QUE gira
const char* motivoActual() {
  if (!modoAuto)                           return "manual";
  if (alarmaHum)                           return "humedad";
  if (enlaceA && sensorAOk && alarmaTempA) return "temperaturaA";
  if (!sensorOk || !enlaceA || !sensorAOk) return "preventivo";
  return "normal";
}

// =====================================================================
// MQTT: PUBLICACIONES
// =====================================================================
void publicarEstado() {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  doc["nodo"] = "B";
  if (sensorOk) doc["hum"] = roundf(ultimaHum * 10) / 10.0f;
  else          doc["hum"] = nullptr;
  doc["sensorOk"]      = sensorOk;
  doc["alarmaHum"]     = alarmaHum;
  doc["extractor"]     = velocidadActual();
  doc["motivo"]        = motivoActual();
  doc["modo"]          = modoAuto ? "auto" : "manual";
  doc["umbralHum"]     = umbralHum;
  doc["histeresisHum"] = histeresisHum;
  doc["intervaloMs"]   = intervaloMs;
  doc["enlaceA"]       = enlaceA;       // estado del enlace ESP-NOW visto por B
  doc["alarmaTempA"]   = alarmaTempA;
  doc["perdidosA"]     = perdidosA;
  doc["rssi"]          = WiFi.RSSI();
  doc["reinicio"]      = motivoReinicio;
  doc["uptimeS"]       = millis() / 1000;

  char buffer[448];
  size_t n = serializeJson(doc, buffer, sizeof(buffer));
  mqtt.publish(T_ESTADO, (const uint8_t*)buffer, n, true);  // retenido
}

void publicarAck(const char* id, bool ok, const char* error) {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  doc["id"]   = id;
  doc["nodo"] = "B";
  doc["ok"]   = ok;
  if (error) doc["error"] = error;

  char buffer[192];
  size_t n = serializeJson(doc, buffer, sizeof(buffer));
  mqtt.publish(T_ACK, (const uint8_t*)buffer, n, false);
}

// =====================================================================
// MQTT: COMANDOS
// Formato: {"id":"c42","accion":"...", ...}
//   extractor -> "valor": 0 o de 40 a 100     (pasa a modo manual)
//   modo      -> "valor": auto | manual
//   set       -> umbralHum (20 a 95), histeresisHum (0 a 20), intervaloMs (2000 a 60000)
//   reporte   -> solo publica el estado
// =====================================================================
bool ejecutarComando(JsonDocument& doc, const char*& error) {
  const char* accion = doc["accion"] | "";

  if (strcmp(accion, "extractor") == 0) {
    if (!doc["valor"].is<int>()) { error = "valor de extractor debe ser entero"; return false; }
    int v = doc["valor"];
    // El hardware no gira de forma confiable bajo el minimo: se rechaza
    if (v != 0 && (v < EXTRACTOR_MIN || v > 100)) {
      error = "extractor fuera de rango (0 o de 40 a 100)";
      return false;
    }
    modoAuto        = false;   // un comando manual toma el control del actuador
    extractorManual = (uint8_t)v;
    return true;
  }

  if (strcmp(accion, "modo") == 0) {
    const char* valor = doc["valor"] | "";
    if (strcmp(valor, "auto") == 0) {
      modoAuto = true;
    } else if (strcmp(valor, "manual") == 0) {
      if (modoAuto) extractorManual = velocidadAuto();  // conserva la salida actual
      modoAuto = false;
    } else {
      error = "valor de modo invalido (auto, manual)";
      return false;
    }
    return true;
  }

  if (strcmp(accion, "set") == 0) {
    // Se valida todo antes de aplicar: o cambian todas o ninguna
    float    nUmbral    = umbralHum;
    float    nHist      = histeresisHum;
    uint32_t nIntervalo = intervaloMs;
    bool     hayCambio  = false;

    if (!doc["umbralHum"].isNull()) {
      if (!doc["umbralHum"].is<float>()) { error = "umbralHum debe ser numerico"; return false; }
      nUmbral = doc["umbralHum"];
      if (nUmbral < 20 || nUmbral > 95) { error = "umbralHum fuera de rango (20 a 95)"; return false; }
      hayCambio = true;
    }
    if (!doc["histeresisHum"].isNull()) {
      if (!doc["histeresisHum"].is<float>()) { error = "histeresisHum debe ser numerica"; return false; }
      nHist = doc["histeresisHum"];
      if (nHist < 0 || nHist > 20) { error = "histeresisHum fuera de rango (0 a 20)"; return false; }
      hayCambio = true;
    }
    if (!doc["intervaloMs"].isNull()) {
      if (!doc["intervaloMs"].is<uint32_t>()) { error = "intervaloMs debe ser entero positivo"; return false; }
      nIntervalo = doc["intervaloMs"];
      if (nIntervalo < 2000 || nIntervalo > 60000) { error = "intervaloMs fuera de rango (2000 a 60000)"; return false; }
      hayCambio = true;
    }
    if (!hayCambio) { error = "set sin variables validas"; return false; }

    umbralHum     = nUmbral;
    histeresisHum = nHist;
    intervaloMs   = nIntervalo;
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
  uint32_t t0 = millis();
  bool ok = mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS,
                         T_CONEXION, 1, true, "offline");
  uint32_t duracion = millis() - t0;
  if (!ok) {
    Serial.printf("fallo (rc=%d) en %lu ms\n", mqtt.state(), (unsigned long)duracion);
    return;
  }
  Serial.printf("conectado en %lu ms\n", (unsigned long)duracion);
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
  if (m.magic != PROTO_MAGIC || m.version != PROTO_VERSION || m.origen != 'A') return;

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
  memcpy(peer.peer_addr, MAC_NODO_A, 6);
  peer.channel = 0;             // 0 = canal actual de la radio
  peer.ifidx   = WIFI_IF_STA;
  peer.encrypt = true;
  memcpy(peer.lmk, ESPNOW_LMK, 16);

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("Error agregando al Nodo A como peer");
    return false;
  }
  return true;
}

// Se llama solo desde el temporizador, con la copia del estado
void enviarHeartbeat(const EstadoSalida& e) {
  MensajeNodo m = {};
  m.magic    = PROTO_MAGIC;
  m.version  = PROTO_VERSION;
  m.origen   = 'B';
  m.seq      = seqTx++;
  m.valor    = e.hum;
  m.alarma   = e.alarma;
  m.sensorOk = e.sensorOk;
  m.actuador = e.velocidad;     // % del extractor
  esp_now_send(MAC_NODO_A, (const uint8_t*)&m, sizeof(m));
}

void procesarMensajeA(uint32_t ahora) {
  if (!rxNuevo) return;

  MensajeNodo m;
  portENTER_CRITICAL(&muxRx);
  m = rxBuf;
  rxNuevo = false;
  portEXIT_CRITICAL(&muxRx);

  // Si la secuencia salta hubo mensajes perdidos.
  // Si retrocede, A se reinicio y se toma como nuevo inicio.
  if (!primerRxA && m.seq > seqEsperadoA) perdidosA += m.seq - seqEsperadoA;
  primerRxA    = false;
  seqEsperadoA = m.seq + 1;

  bool cambio = !enlaceA || (alarmaTempA != (m.alarma == 1)) || (sensorAOk != (m.sensorOk == 1));

  tUltimoRxA  = ahora;
  enlaceA     = true;
  tempA       = m.valor;
  alarmaTempA = (m.alarma == 1);
  sensorAOk   = (m.sensorOk == 1);

  if (cambio) publicarEstado();
}

void verificarEnlaceA(uint32_t ahora) {
  if (enlaceA && ahora - tUltimoRxA > TIMEOUT_ENLACE_MS) {
    enlaceA     = false;
    alarmaTempA = false;   // no se confia en un dato viejo
    Serial.println("Enlace con A perdido: ventilacion preventiva");
    publicarEstado();
  }
}

// =====================================================================
// TEMPORIZADOR DE SALIDAS (EXTRACTOR Y HEARTBEAT)
// Corre en la tarea de esp_timer, fuera del loop. Por eso:
//   - no llama a Serial ni a nada que pueda bloquear
//   - no lee variables del loop, solo la copia "salida"
// =====================================================================
void actualizarSalida() {
  EstadoSalida nueva;
  nueva.velocidad = velocidadActual();
  nueva.hum       = sensorOk ? ultimaHum : NAN;
  nueva.alarma    = alarmaHum ? 1 : 0;
  nueva.sensorOk  = sensorOk ? 1 : 0;

  portENTER_CRITICAL(&muxSalida);
  // Si cambio algo que A usa para decidir, se le avisa sin esperar el periodo
  bool cambioParaA  = (nueva.alarma != salida.alarma) || (nueva.sensorOk != salida.sensorOk);
  nueva.hbInmediato = salida.hbInmediato || cambioParaA;
  salida = nueva;
  portEXIT_CRITICAL(&muxSalida);
}

void tickSalida(void* arg) {
  EstadoSalida e;
  portENTER_CRITICAL(&muxSalida);
  e = salida;
  salida.hbInmediato = false;
  portEXIT_CRITICAL(&muxSalida);

  // Extractor: al pasar de apagado a encendido se aplica un impulso al 100 %
  if (e.velocidad == 0) {
    ticksKick = 0;
  } else if (velAplicada == 0) {
    ticksKick = KICK_MS / TICK_SALIDA_MS;
  }
  velAplicada = e.velocidad;

  uint32_t duty;
  if (ticksKick > 0) {
    ticksKick--;
    duty = PWM_MAX;
  } else {
    duty = dutyDePorcentaje(e.velocidad);
  }
  if (duty != dutyEscrito) {   // solo se escribe cuando cambia
    pwmEscribir(duty);
    dutyEscrito = duty;
  }

  // Heartbeat hacia A: cada HEARTBEAT_MS o de inmediato si lo pidio el loop
  if (++ticksHeartbeat >= HEARTBEAT_MS / TICK_SALIDA_MS || e.hbInmediato) {
    ticksHeartbeat = 0;
    enviarHeartbeat(e);
  }
}

bool iniciarTimerSalida() {
  esp_timer_create_args_t args = {};
  args.callback        = &tickSalida;
  args.arg             = nullptr;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name            = "salidaB";

  if (esp_timer_create(&args, &timerSalida) != ESP_OK) {
    Serial.println("Error creando el temporizador de salidas");
    return false;
  }
  if (esp_timer_start_periodic(timerSalida, (uint64_t)TICK_SALIDA_MS * 1000ULL) != ESP_OK) {
    Serial.println("Error arrancando el temporizador de salidas");
    return false;
  }
  return true;
}

// =====================================================================
// COMPORTAMIENTO SEGURO SIN PLATAFORMA (igual que en A)
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
  motivoReinicio = textoReinicio(esp_reset_reason());
  Serial.printf("Motivo del ultimo reinicio: %s\n", motivoReinicio);
  pinMode(PIN_EXTRACTOR, OUTPUT);
  digitalWrite(PIN_EXTRACTOR, LOW);   // apagado mientras se configura
  if (!pwmIniciar()) Serial.println("Error configurando el PWM del extractor");
  pwmEscribir(0);

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

  actualizarSalida();     // primer estado valido antes de arrancar el temporizador
  iniciarTimerSalida();

  enableLoopWDT();        // si el loop se traba ~5 s el ESP32 se reinicia
}

void loop() {
  // 1. Plataforma (MQTT). Puede bloquear ~3 s si el broker no responde.
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) reconectarMqtt();
    else mqtt.loop();
  }

  uint32_t ahora = millis();

  // 2. Enlace directo con A (ESP-NOW)
  procesarMensajeA(ahora);
  verificarEnlaceA(ahora);

  // 3. Sensor
  if (ahora - tUltimaLectura >= intervaloMs) {
    tUltimaLectura = ahora;
    leerSensor();
    publicarEstado();
  }

  // 4. Seguridad y estado para el temporizador
  gestionarModoSeguro(ahora);
  actualizarSalida();
}
