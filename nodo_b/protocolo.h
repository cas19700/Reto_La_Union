/*
  PROTOCOLO ESP-NOW ENTRE NODOS
  Este archivo debe ser IDENTICO en el Nodo A y en el Nodo B.
  Cualquier cambio aqui se copia a ambas carpetas.
*/
#pragma once
#include <Arduino.h>

// ---------- Identificacion del protocolo ----------
const uint8_t PROTO_MAGIC   = 0xB5;  // byte fijo: descarta mensajes ajenos
const uint8_t PROTO_VERSION = 1;     // si cambia la estructura se sube la version

// ---------- Tiempos del enlace ----------
const uint32_t HEARTBEAT_MS      = 1000;  // cada nodo envia su estado cada 1 s
const uint32_t TIMEOUT_ENLACE_MS = 3500;  // ~3 heartbeats perdidos = enlace caido

// ---------- Red ----------
// ESP-NOW y WiFi comparten la misma radio: ambos nodos deben estar
// en el mismo canal que el router. Fija este canal en el router.
const uint8_t WIFI_CANAL = 6;

// ---------- Direcciones MAC ----------
// Cada nodo imprime su MAC al arrancar en el Monitor Serie.
// Copia aqui las dos y usa el mismo archivo en ambos nodos.
const uint8_t MAC_NODO_A[6] = {0xEC, 0x62, 0x60, 0x9A, 0xF5, 0xF8};
const uint8_t MAC_NODO_B[6] = {0xC0, 0xCD, 0xD6, 0xD0, 0x05, 0xF8};

// ---------- Cifrado ESP-NOW ----------
// Las claves PMK y LMK viven en secretos.h (no se suben a Git).
// Ese archivo tambien debe ser IDENTICO en ambos nodos.

// ---------- Estructura del mensaje (14 bytes) ----------
// packed: sin bytes de relleno para que A y B lean exactamente lo mismo
struct __attribute__((packed)) MensajeNodo {
  uint8_t  magic;     // PROTO_MAGIC
  uint8_t  version;   // PROTO_VERSION
  char     origen;    // 'A' o 'B'
  uint32_t seq;       // contador para detectar mensajes perdidos
  float    valor;     // A: temperatura en C | B: humedad relativa en %
  uint8_t  alarma;    // 1 si el origen tiene su alarma activa
  uint8_t  sensorOk;  // 0 si el sensor del origen esta fallando
  uint8_t  actuador;  // A: patron del LED (0-3) | B: % del extractor (0-100)
};
