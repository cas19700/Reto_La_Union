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

Pines del Nodo A: DHT22 en GPIO 4 y LED en GPIO 18. Se eligieron porque no son pines de arranque (0, 2, 5, 12 y 15), no son solo de entrada (34 a 39) y no están conectados a la flash (6 a 11).

Conexión del Nodo A: LED rojo con resistencia de 220 ohm entre GPIO 18 y GND. Con una caída de 1.8 a 2.0 V en el LED circulan de 5.9 a 6.8 mA, lejos del máximo absoluto de 40 mA de un GPIO. El DHT22 se alimenta desde 3V3 y no desde VIN porque los GPIO del ESP32 no toleran 5 V en la línea de datos. Lleva pull-up de 10 kohm entre DATA y 3V3 (se omite si el sensor viene en módulo de 3 pines porque ya la trae) y un capacitor de 100 nF entre VCC y GND junto al sensor para filtrar los picos de consumo del WiFi.

La placa del Nodo A tiene un chip ESP32-D0WDQ6 revisión v1.0 según esptool. Es una revisión antigua. Sus erratas conocidas afectan a la PSRAM y al arranque seguro con cifrado de flash y ninguno de los dos se usa. Falta revisar la revisión de las otras dos placas.

## Arquitectura

**Entre nodos: ESP-NOW.** Enlace directo sin router ni broker para cumplir la restricción 3 sin ambigüedad. Unicast por dirección MAC con cifrado (claves PMK y LMK de 16 caracteres guardadas en secretos.h). Cada nodo envía un heartbeat con su estado cada 1 s y de inmediato cuando cambia su alarma o el estado de su sensor. Si no llega nada en 3.5 s el enlace se declara caído.

**Temporizador de salidas (esp_timer).** En el Nodo A el LED y el heartbeat no los maneja el loop sino un temporizador de ESP-IDF que se ejecuta cada 50 ms en su propia tarea. El loop decide qué patrón mostrar y qué datos enviar a B y deja esa decisión en una estructura compartida protegida con una sección crítica (spinlock). El temporizador solo lee esa copia y la ejecuta. Así el LED y el enlace con B siguen funcionando aunque el loop se bloquee. El callback no llama a Serial ni a nada que pueda bloquear. Los periodos del LED y del heartbeat deben ser múltiplos de 50 ms y el compilador lo verifica con static_assert.

**Hacia la plataforma: MQTT.** Broker Mosquitto en la laptop con usuario y clave. Keepalive de 5 s y Last Will en el tópico de conexión ("offline" retenido). Plataforma en Node-RED Dashboard accesible desde laptop y celular.

**Canal WiFi fijo en 6.** ESP-NOW y WiFi comparten la radio del ESP32 y ambos nodos deben estar en el mismo canal que el router. El ESP32 solo usa 2.4 GHz. El router de pruebas es de doble banda con el mismo nombre en ambas y su red de 2.4 GHz ya está en el canal 6 (verificado con netsh wlan show networks mode=bssid). La laptop puede quedar en 5 GHz porque ambas bandas salen del mismo router y están en la misma red local. Falta confirmar en el router que el canal esté fijo y no en automático. Para la evaluación conviene llevar un router propio o un hotspot en 2.4 GHz donde se controle el canal.

## Protocolo ESP-NOW (archivos protocolo.h y secretos.h idénticos en ambos nodos)

Estructura binaria packed de 14 bytes: magic (0xB5), versión (1), origen ('A' o 'B'), seq (contador para detectar pérdidas), valor (temperatura en A o humedad en B), alarma (0/1), sensorOk (0/1) y actuador (patrón del LED en A o porcentaje del extractor en B). Los mensajes con magic, versión, tamaño u origen incorrectos se descartan.

## Protocolo MQTT

Tópicos por nodo (ejemplo con A): `nodoA/estado` (retenido), `nodoA/ack`, `nodoA/conexion` (Last Will), `nodoA/cmd`. Tópico común: `todos/cmd` para comandos simultáneos.

Todo comando lleva un `id`. El nodo responde en su tópico ack con `{"id":"c42","nodo":"A","ok":true}` o con `ok` falso y el campo `error`. Después publica su estado real. Un `id` repetido no se ejecuta de nuevo y se reenvía la respuesta anterior.

**Firewall de Windows.** Por defecto descarta en silencio las conexiones entrantes al puerto 1883. Hay que crear una regla de entrada para Mosquitto o los nodos no podrán conectarse aunque el broker esté funcionando.

Comandos del Nodo A:
`led` con valor on, off o blink (pasa a modo manual). `modo` con valor auto o manual. `set` con umbralTemp (-40 a 80), histeresis (0 a 10) e intervaloMs (2000 a 60000); se valida todo antes de aplicar y si algo es inválido se rechaza el comando completo. `reporte` solo publica el estado.

Las variables de operación se guardan en flash con la librería Preferences.

## Comportamiento del Nodo A

LED apagado significa normal. Parpadeo rápido (250 ms) significa alarma de temperatura en A o de humedad en B. Parpadeo lento (1 s) significa falla del sensor de A, del sensor de B o del enlace con B. La alarma tiene prioridad sobre la falla.

La alarma de temperatura usa histéresis. El sensor se declara en falla tras tres lecturas fallidas seguidas.

El LED y el heartbeat los ejecuta el temporizador de salidas. Cualquier cambio de la alarma o del estado del sensor de A (por una lectura o por un comando set que cambia el umbral) dispara un heartbeat inmediato hacia B en menos de 50 ms.

**Comportamiento seguro:** si se pierde el enlace con B se descarta su última alarma y se muestra falla. Si el nodo está en manual y pierde la plataforma por 30 s regresa solo a automático. Watchdog del loop activado con enableLoopWDT (reinicia si el loop se traba unos 5 s).

## Limitaciones conocidas (declararlas en la presentación)

Si cae el router los nodos buscan la red en todos los canales y el enlace ESP-NOW se vuelve intermitente. Entra el comportamiento seguro.

Si solo cae el broker o la laptop el enlace entre nodos sigue funcionando. Pero mqtt.connect() de PubSubClient bloquea el loop hasta que vence el tiempo de espera de conexión TCP del core. Medido en la placa: 3002 ms por intento con un intento cada 5 s (el loop queda detenido el 60 % del tiempo). Antes del temporizador de salidas el LED se congelaba y el heartbeat hacia B podía retrasarse hasta unos 4 s. Eso supera el timeout de 3.5 s así que B habría declarado el enlace caído en cerca de la mitad de los intentos. Con el temporizador el LED y el heartbeat ya no dependen del loop. Durante el bloqueo siguen detenidos el procesamiento de comandos y las lecturas del sensor (se retrasan hasta 3 s) y la detección de pérdida de B puede retrasarse unos 3 s más de lo normal. La solución completa sería el cliente MQTT asíncrono de ESP-IDF (esp_mqtt). Se descartó por el riesgo de reescribir la parte MQTT cerca de la entrega y queda como mejora futura.

Una lectura fallida del DHT22 tarda unos 73 ms (medido sin sensor conectado) porque la librería de Adafruit cuenta iteraciones y no tiempo real para su límite de espera. No afecta al LED ni al heartbeat.

## Elementos opcionales elegidos

Cifrado de ESP-NOW y autenticación en el broker con validación de rangos (ya implementados en A). Reconexión automática (implementada). Registro histórico de lecturas y comandos en Node-RED con SQLite o CSV. Alarmas fuera de rango. Actualización OTA con ArduinoOTA.

## Credenciales y repositorio

Las credenciales se separan del código en un archivo secretos.h que no se sube a GitHub (está en .gitignore). Contiene el SSID y la clave del WiFi, la IP del broker, las claves MQTT de ambos nodos (MQTT_PASS_NODO_A y MQTT_PASS_NODO_B) y las claves PMK y LMK de ESP-NOW. Es idéntico en ambos nodos igual que protocolo.h. Cada nodo toma su propia clave MQTT. Los usuarios MQTT, los tópicos y el puerto no son secretos y quedan en el .ino. En el repositorio se sube secretos_ejemplo.h con valores falsos como plantilla.

Si falta secretos.h el código no compila y muestra un mensaje que explica cómo crearlo (uso de __has_include). Si una clave de ESP-NOW no tiene exactamente 16 caracteres tampoco compila (static_assert).

Limitación: cada nodo guarda en su flash la clave MQTT del otro nodo. Como no se usa cifrado de flash quien lea la memoria de un nodo obtiene todas las credenciales. Es aceptable para un prototipo y se declara en la presentación.

Repositorio en GitHub con carpetas nodo_a, nodo_b, node-red y docs. El PDF del enunciado no se publica por ser un documento de la empresa.

## Entorno

Código en C++ con el framework de Arduino en Arduino IDE 2.3.10. Librerías: PubSubClient 2.8, DHT sensor library 1.4.6 de Adafruit con Adafruit Unified Sensor 1.1.14 y ArduinoJson 7.4.2. El Nodo A versión 3 compila sin advertencias (Compiler warnings en "All") con los cores del ESP32 2.0.9 y 3.3.12. El core instalado en la laptop es el 3.3.12.

El Nodo A imprime en el monitor serie cuánto tarda cada intento de conexión MQTT y cada lectura fallida del DHT22. Sirve como registro para diagnosticar la red y el sensor.

## Estado del trabajo

Nodo A versión 3: LED y heartbeat en un temporizador de salidas. Compilado con los cores 2.0.9 y 3.3.12. La versión 2 se probó en la placa: se conecta al WiFi y con ella se midió el bloqueo de MQTT. Falta probar la versión 3 en la placa (el LED debe parpadear parejo aunque falle MQTT) y conectar el DHT22 cuando llegue. Falta leer la MAC de la placa del Nodo B y copiar ambas MAC en protocolo.h. Falta instalar Mosquitto y abrir el puerto 1883 en el firewall. Pendiente: Nodo B (definir su estado seguro y sus variables remotas), documento formal del protocolo, plataforma Node-RED, opcionales y preparación de la presentación.

## Cronograma

25 al 27 de septiembre: compra de hardware y pruebas de cada sensor y actuador por separado. 28 al 30: comunicación MQTT y ESP-NOW entre nodos. 1 y 2 de octubre: lógica cruzada, comandos y variables. 3: dashboard en Node-RED. 4: robustez y opcionales. 5: documentación y ensayo de la presentación. 6: entrega.
