/*
  CREDENCIALES REALES - NO SUBIR A GIT (esta en .gitignore)
  Debe ser IDENTICO en el Nodo A y en el Nodo B.
  Pendiente: IP real de la laptop y claves MQTT cuando se instale Mosquitto.
*/
#pragma once
#include <Arduino.h>

// ---------- WiFi (router en el canal WIFI_CANAL de protocolo.h) ----------
const char WIFI_SSID[]     = "TIGO-D4F5";
const char WIFI_PASSWORD[] = "4G79ED805952";

// ---------- Broker MQTT (Mosquitto en la laptop) ----------
const char MQTT_BROKER[]      = "192.168.0.5";  // PENDIENTE: IPv4 de la laptop (ipconfig)
const char MQTT_PASS_NODO_A[] = "nodoa2026";   // PENDIENTE: se definen al crear los usuarios en Mosquitto
const char MQTT_PASS_NODO_B[] = "nodob2026";   // PENDIENTE: se definen al crear los usuarios en Mosquitto 
// PC = central2026
// ---------- Cifrado ESP-NOW: exactamente 16 caracteres cada una ----------
const char ESPNOW_PMK[] = "ZH1inaXVyvM6F272";
const char ESPNOW_LMK[] = "yBiuvbOLYMasGD6G";
