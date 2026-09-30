# Reto IoT: supervisión de bodega de azúcar

Prototipo de dos nodos IoT con ESP32 que se comunican entre sí por ESP-NOW y con una plataforma de usuario en Node-RED por MQTT. Desarrollado como reto técnico para la posición de Ingeniero Electrónico IoT en Ingenio La Unión.

El azúcar absorbe humedad y se apelmaza. Por eso el sistema vigila la temperatura y la humedad de una bodega y actúa sobre la ventilación.


## Nodos

**Nodo A (panel de supervisión).** ESP32 con DHT22 para temperatura y un LED de alarma. El LED apagado indica operación normal. Parpadeo rápido indica alarma y parpadeo lento indica falla de un sensor o del enlace.

**Nodo B (zona de bodega).** ESP32 con DHT22 para humedad y un extractor de 5 V controlado por PWM. En desarrollo.

Los nodos se comunican directamente por ESP-NOW cifrado sin depender de la plataforma. Una temperatura alta en A aumenta la ventilación en B. Una humedad alta en B enciende el extractor y activa la alarma en A.

## Estructura

```
nodo_a/     firmware del Nodo A
nodo_b/     firmware del Nodo B
node-red/   flujo exportado de la plataforma
docs/       decisiones técnicas y diagramas
```

`protocolo.h` define el mensaje ESP-NOW y debe ser idéntico en ambos nodos.

## Cómo compilar

1. En la carpeta de cada nodo copia `secretos_ejemplo.h` con el nombre `secretos.h` y llénalo con los valores reales. Ese archivo no se sube al repositorio y debe ser idéntico en ambos nodos.
2. Copia en `protocolo.h` las direcciones MAC que cada nodo imprime al arrancar.
3. En Arduino IDE selecciona la placa "ESP32 Dev Module".
4. Instala las librerías PubSubClient, DHT sensor library de Adafruit con Adafruit Unified Sensor y ArduinoJson 7.

Compatible con los cores del ESP32 2.x y 3.x (verificado con 2.0.9 y 3.3.0).

