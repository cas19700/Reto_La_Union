/*
  PLANTILLA DE CREDENCIALES
  1. Copia este archivo en la misma carpeta con el nombre secretos.h
  2. Llena secretos.h con los valores reales
  3. secretos.h debe ser IDENTICO en el Nodo A y en el Nodo B
  secretos.h esta en .gitignore y nunca se sube al repositorio.
  Este archivo de ejemplo si se sube: solo contiene valores falsos.
*/
#pragma once
#include <Arduino.h>

// ---------- WiFi (router en el canal WIFI_CANAL de protocolo.h) ----------
const char WIFI_SSID[]     = "TU_RED";
const char WIFI_PASSWORD[] = "TU_CLAVE_WIFI";

// ---------- Broker MQTT (Mosquitto en la laptop) ----------
const char MQTT_BROKER[]      = "192.168.1.100";
const char MQTT_PASS_NODO_A[] = "CLAVE_MQTT_A";
const char MQTT_PASS_NODO_B[] = "CLAVE_MQTT_B";

// ---------- Cifrado ESP-NOW: exactamente 16 caracteres cada una ----------
const char ESPNOW_PMK[] = "CAMBIAR_PMK_16ch";
const char ESPNOW_LMK[] = "CAMBIAR_LMK_16ch";
