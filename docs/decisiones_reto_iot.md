# Reto IoT Ingenio La Unión: decisiones técnicas

## Contexto

Reto técnico para la posición de Ingeniero Electrónico IoT en Ingenio La Unión. Entrega tentativa el 6 de octubre de 2026. El hardware lo consigo yo. Debo poder explicar y defender cada decisión durante la evaluación y los días siguientes. El enunciado completo está en el PDF de este proyecto.

## Escenario elegido

Bodega de almacenamiento de azúcar. El azúcar absorbe humedad y se apelmaza así que controlar temperatura y humedad en bodega es un problema real del negocio.

**Nodo A (panel de supervisión):** ESP32 + DHT22 (temperatura, sensor real) + LED de alarma.

**Nodo B (zona de bodega):** ESP32 + DHT22 (humedad, sensor real) + extractor (ventilador 5 V de 40 mm) controlado con transistor NPN 2N2222A, diodo flyback Schottky 1N5822 y PWM para la velocidad. Se usa el 2N2222A porque la tienda no tiene MOSFET de nivel lógico. Base con resistencia de 220 ohm (unos 11 mA de base) y pull-down de 10 kohm entre base y emisor para que el ventilador no arranque solo mientras el ESP32 inicia.

**Interacciones entre nodos (sin plataforma):** temperatura alta en A hace que B aumente la ventilación. Humedad alta en B enciende el extractor en B y activa la alarma en A.

## Hardware

Tres módulos ESP32 de 30 pines con ESP-WROOM-32, chip CP2102 y USB-C (Electrónica Tettsa, SKU MO-ESP32-C01; dos nodos y uno de repuesto). En Arduino IDE se selecciona "ESP32 Dev Module". Dos DHT22, LED con resistencia de 220 ohm, ventilador 5 VDC 40 x 40 x 10 mm, transistor 2N2222A, diodo 1N5822, resistencias de 220 ohm y 10 kohm, capacitores de 100 uF y 100 nF, cuatro protoboards de 800 puntos, jumpers y cables USB-C de datos. Cotización enviada a Electrónica Tettsa.

Pines del Nodo A: DHT22 en GPIO 4 y LED en GPIO 18.

## Arquitectura

**Entre nodos: ESP-NOW.** Enlace directo sin router ni broker para cumplir la restricción 3 sin ambigüedad. Unicast por dirección MAC con cifrado (claves PMK y LMK de 16 caracteres guardadas en secretos.h). Cada nodo envía un heartbeat con su estado cada 1 s y de inmediato cuando cambia su alarma. Si no llega nada en 3.5 s el enlace se declara caído.

**Hacia la plataforma: MQTT.** Broker Mosquitto en la laptop con usuario y clave. Keepalive de 5 s y Last Will en el tópico de conexión ("offline" retenido). Plataforma en Node-RED Dashboard accesible desde laptop y celular.

**Canal WiFi fijo en 6.** ESP-NOW y WiFi comparten la radio del ESP32 y ambos nodos deben estar en el mismo canal que el router.

## Protocolo ESP-NOW (archivos protocolo.h y secretos.h idénticos en ambos nodos)

Estructura binaria packed de 14 bytes: magic (0xB5), versión (1), origen ('A' o 'B'), seq (contador para detectar pérdidas), valor (temperatura en A o humedad en B), alarma (0/1), sensorOk (0/1) y actuador (patrón del LED en A o porcentaje del extractor en B). Los mensajes con magic, versión, tamaño u origen incorrectos se descartan.

## Protocolo MQTT

Tópicos por nodo (ejemplo con A): `nodoA/estado` (retenido), `nodoA/ack`, `nodoA/conexion` (Last Will), `nodoA/cmd`. Tópico común: `todos/cmd` para comandos simultáneos.

Todo comando lleva un `id`. El nodo responde en su tópico ack con `{"id":"c42","nodo":"A","ok":true}` o con `ok` falso y el campo `error`. Después publica su estado real. Un `id` repetido no se ejecuta de nuevo y se reenvía la respuesta anterior.

Comandos del Nodo A:
`led` con valor on, off o blink (pasa a modo manual). `modo` con valor auto o manual. `set` con umbralTemp (-40 a 80), histeresis (0 a 10) e intervaloMs (2000 a 60000); se valida todo antes de aplicar y si algo es inválido se rechaza el comando completo. `reporte` solo publica el estado.

Las variables de operación se guardan en flash con la librería Preferences.

## Comportamiento del Nodo A

LED apagado significa normal. Parpadeo rápido (250 ms) significa alarma de temperatura en A o de humedad en B. Parpadeo lento (1 s) significa falla del sensor de A, del sensor de B o del enlace con B. La alarma tiene prioridad sobre la falla.

La alarma de temperatura usa histéresis. El sensor se declara en falla tras tres lecturas fallidas seguidas.

**Comportamiento seguro:** si se pierde el enlace con B se descarta su última alarma y se muestra falla. Si el nodo está en manual y pierde la plataforma por 30 s regresa solo a automático. Watchdog del loop activado con enableLoopWDT (reinicia si el loop se traba unos 5 s).

## Limitación conocida (declararla en la presentación)

Si cae el router los nodos buscan la red en todos los canales y el enlace ESP-NOW se vuelve intermitente. Entra el comportamiento seguro. Si solo cae el broker o la laptop el enlace entre nodos sigue funcionando.

## Elementos opcionales elegidos

Cifrado de ESP-NOW y autenticación en el broker con validación de rangos (ya implementados en A). Reconexión automática (implementada). Registro histórico de lecturas y comandos en Node-RED con SQLite o CSV. Alarmas fuera de rango. Actualización OTA con ArduinoOTA.

## Credenciales y repositorio

Las credenciales se separan del código en un archivo secretos.h que no se sube a GitHub (está en .gitignore). Contiene el SSID y la clave del WiFi, la IP del broker, las claves MQTT de ambos nodos (MQTT_PASS_NODO_A y MQTT_PASS_NODO_B) y las claves PMK y LMK de ESP-NOW. Es idéntico en ambos nodos igual que protocolo.h. Cada nodo toma su propia clave MQTT. Los usuarios MQTT, los tópicos y el puerto no son secretos y quedan en el .ino. En el repositorio se sube secretos_ejemplo.h con valores falsos como plantilla.

Si falta secretos.h el código no compila y muestra un mensaje que explica cómo crearlo (uso de __has_include). Si una clave de ESP-NOW no tiene exactamente 16 caracteres tampoco compila (static_assert).

Limitación: cada nodo guarda en su flash la clave MQTT del otro nodo. Como no se usa cifrado de flash quien lea la memoria de un nodo obtiene todas las credenciales. Es aceptable para un prototipo y se declara en la presentación.

Repositorio en GitHub con carpetas nodo_a, nodo_b, node-red y docs. El PDF del enunciado no se publica por ser un documento de la empresa.

## Entorno

Código en C++ con el framework de Arduino. Librerías: PubSubClient, DHT sensor library de Adafruit con Adafruit Unified Sensor y ArduinoJson 7. El Nodo A compila sin advertencias con los cores del ESP32 2.0.9 y 3.3.0.

## Estado del trabajo

Nodo A versión 2: terminado y compilado. Credenciales separadas en secretos.h y verificado de nuevo con los cores 2.0.9 y 3.3.0. Pendiente: Nodo B (definir su estado seguro y sus variables remotas), documento formal del protocolo, plataforma Node-RED, opcionales y preparación de la presentación.

## Cronograma

25 al 27 de septiembre: compra de hardware y pruebas de cada sensor y actuador por separado. 28 al 30: comunicación MQTT y ESP-NOW entre nodos. 1 y 2 de octubre: lógica cruzada, comandos y variables. 3: dashboard en Node-RED. 4: robustez y opcionales. 5: documentación y ensayo de la presentación. 6: entrega.
