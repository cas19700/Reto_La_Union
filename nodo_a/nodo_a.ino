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

  Tareas:
    - loop():        sensor, comandos MQTT, enlace con B y logica del nodo
    - temporizador:  LED y heartbeat hacia B cada 50 ms (esp_timer)
    Si mqtt.connect() bloquea el loop ~3 s porque el broker no responde,
    el LED sigue su patron y B sigue recibiendo el heartbeat.

  Credenciales: secretos.h (no se sube a Git, ver secretos_ejemplo.h)

  Librerias: PubSubClient, DHT sensor library + Adafruit Unified Sensor, ArduinoJson 7
*/

#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_timer.h>
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
const uint32_t TICK_SALIDA_MS            = 50;     // periodo del temporizador de salidas
const uint32_t PERIODO_RAPIDO_MS         = 250;
const uint32_t PERIODO_LENTO_MS          = 1000;
const uint32_t REINTENTO_MQTT_MS         = 5000;
const uint32_t MANUAL_SIN_SUPERVISION_MS = 30000;  // manual sin plataforma -> auto
const uint8_t  FALLOS_SENSOR_MAX         = 3;      // lecturas fallidas seguidas

// El temporizador cuenta ticks enteros: los periodos deben ser multiplos del tick
static_assert(PERIODO_RAPIDO_MS % TICK_SALIDA_MS == 0, "PERIODO_RAPIDO_MS no es multiplo del tick");
static_assert(PERIODO_LENTO_MS  % TICK_SALIDA_MS == 0, "PERIODO_LENTO_MS no es multiplo del tick");
static_assert(HEARTBEAT_MS      % TICK_SALIDA_MS == 0, "HEARTBEAT_MS no es multiplo del tick");

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

// El callback de ESP-NOW corre en otra tarea: solo copia el mensaje
// y el loop lo procesa despues (misma idea que una ISR corta)
portMUX_TYPE muxRx = portMUX_INITIALIZER_UNLOCKED;
MensajeNodo rxBuf;
volatile bool rxNuevo = false;

// ================= SALIDAS DEL TEMPORIZADOR =================
// El loop decide QUE mostrar y QUE enviar a B y lo deja en "salida".
// El temporizador solo lee esa copia y la ejecuta. Asi nunca lee
// variables del loop a medias y no depende de que el loop este libre.
struct EstadoSalida {
  PatronLed patron;       // patron que debe mostrar el LED
  float     temp;         // temperatura para B (NAN si el sensor falla)
  uint8_t   alarma;       // 1 si A tiene alarma de temperatura
  uint8_t   sensorOk;     // 1 si el sensor de A funciona
  bool      hbInmediato;  // pide un heartbeat sin esperar el periodo
};
portMUX_TYPE muxSalida = portMUX_INITIALIZER_UNLOCKED;
EstadoSalida salida = {LED_LENTO, NAN, 0, 0, false};
esp_timer_handle_t timerSalida = nullptr;

// Estas solo las usa el temporizador (el loop no las toca)
uint32_t seqTx          = 0;
bool     ledFisico      = false;
uint32_t ticksParpadeo  = 0;
uint32_t ticksHeartbeat = 0;

// ================= TIEMPOS Y COMANDOS =================
uint32_t tUltimaLectura     = 0;
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
  uint32_t t0 = millis();
  float t = dht.readTemperature();
  uint32_t duracion = millis() - t0;
  if (isnan(t)) {
    // Una lectura fallida aislada es normal en el DHT22: solo se declara
    // falla despues de varias seguidas
    if (fallosSensor < 255) fallosSensor++;
    if (fallosSensor >= FALLOS_SENSOR_MAX) sensorOk = false;
    Serial.printf("Lectura DHT22 fallida (%u seguidas) en %lu ms\n", fallosSensor, (unsigned long)duracion);
  } else {
    fallosSensor = 0;
    sensorOk = true;
    ultimaTemp = t;
    Serial.printf("Temperatura: %.1f C (lectura en %lu ms)\n", t, (unsigned long)duracion);
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

// Se llama solo desde el temporizador, con la copia del estado
void enviarHeartbeat(const EstadoSalida& e) {
  MensajeNodo m = {};
  m.magic    = PROTO_MAGIC;
  m.version  = PROTO_VERSION;
  m.origen   = 'A';
  m.seq      = seqTx++;
  m.valor    = e.temp;
  m.alarma   = e.alarma;
  m.sensorOk = e.sensorOk;
  m.actuador = (uint8_t)e.patron;
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
// TEMPORIZADOR DE SALIDAS (LED Y HEARTBEAT)
// Corre en la tarea de esp_timer, fuera del loop. Por eso:
//   - no llama a Serial ni a nada que pueda bloquear
//   - no lee variables del loop, solo la copia "salida"
// =====================================================================

// Lo llama el loop en cada vuelta: calcula el estado y lo publica
void actualizarSalida() {
  EstadoSalida nueva;
  nueva.patron   = patronActual();
  nueva.temp     = sensorOk ? ultimaTemp : NAN;
  nueva.alarma   = alarmaTemp ? 1 : 0;
  nueva.sensorOk = sensorOk ? 1 : 0;

  portENTER_CRITICAL(&muxSalida);
  // Si cambio algo que B usa para decidir, se le avisa sin esperar el periodo
  bool cambioParaB  = (nueva.alarma != salida.alarma) || (nueva.sensorOk != salida.sensorOk);
  nueva.hbInmediato = salida.hbInmediato || cambioParaB;
  salida = nueva;
  portEXIT_CRITICAL(&muxSalida);
}

void tickSalida(void* arg) {
  // Copia el estado y baja la bandera de heartbeat inmediato en un solo paso
  EstadoSalida e;
  portENTER_CRITICAL(&muxSalida);
  e = salida;
  salida.hbInmediato = false;
  portEXIT_CRITICAL(&muxSalida);

  // LED
  if (e.patron == LED_OFF) {
    ledFisico = false;
    ticksParpadeo = 0;
  } else if (e.patron == LED_ON) {
    ledFisico = true;
    ticksParpadeo = 0;
  } else {
    uint32_t periodo = (e.patron == LED_RAPIDO) ? PERIODO_RAPIDO_MS : PERIODO_LENTO_MS;
    if (++ticksParpadeo >= periodo / TICK_SALIDA_MS) {
      ticksParpadeo = 0;
      ledFisico = !ledFisico;
    }
  }
  digitalWrite(PIN_LED, ledFisico ? HIGH : LOW);

  // Heartbeat hacia B: cada HEARTBEAT_MS o de inmediato si lo pidio el loop
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
  args.name            = "salidaA";

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

  actualizarSalida();     // primer estado valido antes de arrancar el temporizador
  iniciarTimerSalida();

  enableLoopWDT();        // si el loop se traba ~5 s el ESP32 se reinicia
}

void loop() {
  // 1. Plataforma (MQTT). Si el broker no responde, mqtt.connect() puede
  //    bloquear ~3 s. El LED y el heartbeat no dependen de esto.
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqtt.connected()) reconectarMqtt();
    else mqtt.loop();
  }

  // La hora se toma DESPUES de MQTT para no trabajar con una hora vieja
  uint32_t ahora = millis();

  // 2. Enlace directo con B (ESP-NOW)
  procesarMensajeB(ahora);
  verificarEnlaceB(ahora);

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
