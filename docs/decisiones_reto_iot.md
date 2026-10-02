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

Estado del montaje del Nodo A: LED y DHT22 conectados y probados en la protoboard. La MAC de la placa del Nodo A ya está en protocolo.h.

La placa del Nodo B tiene un chip ESP32-D0WD-V3 revisión v3.1. Es de otra generación que la del Nodo A. Ambas ejecutan el mismo código y ESP-NOW funciona igual entre revisiones. Su MAC ya está en protocolo.h. Mientras tenga cargado el firmware del Nodo A no deben estar encendidas las dos placas a la vez porque usarían el mismo identificador MQTT y el broker las desconectaría una a la otra.

**Pines del Nodo B.** Extractor en GPIO 25. En la protoboard del Nodo B el ESP32 tapa todos los agujeros libres de la fila de pines de 3V3 (la de D4 y D18) así que solo es accesible la fila de VIN. GPIO 25 está en esa fila junto a VIN y GND. No es pin de arranque, no emite señales al encender y acepta PWM. Se descartaron los otros pines de esa fila: 34, 35, 36 y 39 son solo de entrada, 12 es un pin de arranque que impide arrancar si está en alto y 14 emite una señal al encender que haría dar un tirón al ventilador. El DHT22 del Nodo B usa el pin 3V3 de la fila tapada. Se resolvió colocando el ESP32 al borde de la protoboard con esa fila colgando y jumpers hembra-macho (detalle en Estado del trabajo).

**Circuito del extractor.** El ventilador recibe 5 V de VIN por su cable rojo. Su cable negro va al colector del 2N2222A y el emisor va a GND (interruptor del lado de tierra). Base con 220 ohm desde GPIO 25 (unos 11.4 mA con 0.8 V de caída base-emisor) y 10 kohm a GND. Diodo 1N5822 en paralelo con el ventilador con la banda hacia 5 V. Capacitores de 100 uF (pata larga a 5 V) y 100 nF entre 5 V y GND cerca del ventilador. Antes de energizar se verifica el orden de patas del transistor porque las versiones PN2222A (E-B-C) y P2N2222A (C-B-E) en TO-92 lo tienen invertido. El 1N5822 tiene patas de 1.3 mm que no entran bien en la protoboard: se le sueldan cables delgados o se usa un 1N5819.

**Prueba del extractor (30 de septiembre).** Con PWM a 25 kHz el ventilador solo giraba al 100 %. Por debajo arrancaba y se detenía a los pocos segundos. Causa: es un ventilador de dos cables con un controlador interno y un sensor Hall alimentados por los mismos cables del motor. Cada corte del PWM deja sin energía al controlador y su protección contra bloqueo detiene el motor. Los ventiladores de cuatro cables tienen una entrada de PWM separada a 25 kHz justamente por eso. Con PWM de baja frecuencia cada pulso de encendido dura varios milisegundos y el controlador funciona. Resultados: gira estable a 20, 50 y 100 Hz. Mínimo estable 30 %. Por debajo le cuesta y la velocidad es irregular. Arranca solo desde parado al 30 % sin impulso. El transistor sigue frío tras más de un minuto al 100 % (está saturado). Al levantar el ventilador con la mano a veces se traba. En la mesa gira constante.

**Valores elegidos para el firmware del Nodo B.** PWM a 100 Hz (la frecuencia más alta probada que funciona). Mínimo útil de 40 %: el extractor solo puede estar apagado (0 %) o entre 40 y 100 %. La lógica automática nunca pide menos de 40 % y un comando remoto con un valor de 1 a 39 % se rechaza con error. Los 10 puntos de margen cubren la caída de voltaje de VIN cuando el WiFi consume picos de corriente. Impulso de arranque al 100 % durante 500 ms al pasar de apagado a encendido. No hace falta en el banco de pruebas pero protege el arranque con voltaje más bajo o con polvo en el ventilador.

Pendiente: anotar la corriente de la etiqueta del ventilador.

## Arquitectura

**Entre nodos: ESP-NOW.** Enlace directo sin router ni broker para cumplir la restricción 3 sin ambigüedad. Unicast por dirección MAC con cifrado (claves PMK y LMK de 16 caracteres guardadas en secretos.h). Cada nodo envía un heartbeat con su estado cada 1 s y de inmediato cuando cambia su alarma o el estado de su sensor. Si no llega nada en 3.5 s el enlace se declara caído.

**Temporizador de salidas (esp_timer).** En el Nodo A el LED y el heartbeat no los maneja el loop sino un temporizador de ESP-IDF que se ejecuta cada 50 ms en su propia tarea. El loop decide qué patrón mostrar y qué datos enviar a B y deja esa decisión en una estructura compartida protegida con una sección crítica (spinlock). El temporizador solo lee esa copia y la ejecuta. Así el LED y el enlace con B siguen funcionando aunque el loop se bloquee. El callback no llama a Serial ni a nada que pueda bloquear. Los periodos del LED y del heartbeat deben ser múltiplos de 50 ms y el compilador lo verifica con static_assert.

**Hacia la plataforma: MQTT.** Broker Mosquitto en la laptop con usuario y clave. Keepalive de 5 s y Last Will en el tópico de conexión ("offline" retenido). Plataforma en Node-RED Dashboard accesible desde laptop y celular.

**Canal WiFi fijo en 6.** ESP-NOW y WiFi comparten la radio del ESP32 y ambos nodos deben estar en el mismo canal que el router. El ESP32 solo usa 2.4 GHz. El router de pruebas es de doble banda con el mismo nombre en ambas y su red de 2.4 GHz ya está en el canal 6 (verificado con netsh wlan show networks mode=bssid). La laptop puede quedar en 5 GHz porque ambas bandas salen del mismo router y están en la misma red local. Falta confirmar en el router que el canal esté fijo y no en automático. Para la evaluación conviene llevar un router propio o un hotspot en 2.4 GHz donde se controle el canal.

## Protocolo ESP-NOW (archivos protocolo.h y secretos.h idénticos en ambos nodos)

Estructura binaria packed de 14 bytes: magic (0xB5), versión (1), origen ('A' o 'B'), seq (contador para detectar pérdidas), valor (temperatura en A o humedad en B), alarma (0/1), sensorOk (0/1) y actuador (patrón del LED en A o porcentaje del extractor en B). Los mensajes con magic, versión, tamaño u origen incorrectos se descartan.

## Protocolo MQTT

Tópicos por nodo (ejemplo con A): `nodoA/estado` (retenido), `nodoA/ack`, `nodoA/conexion` (Last Will), `nodoA/cmd`. Tópico común: `todos/cmd` para comandos simultáneos.

Todo comando lleva un `id`. El nodo responde en su tópico ack con `{"id":"c42","nodo":"A","ok":true}` o con `ok` falso y el campo `error`. Después publica su estado real. Un `id` repetido no se ejecuta de nuevo y se reenvía la respuesta anterior sin publicar un estado nuevo. El nodo solo recuerda el último `id`. Eso cubre el caso para el que existe (la plataforma reenvía un comando porque no recibió el ack) pero un `id` más antiguo sí se ejecutaría otra vez. Una mejora sería guardar los últimos `id` en un arreglo circular.

Comandos del Nodo A:
`led` con valor on, off o blink (pasa a modo manual). `modo` con valor auto o manual. `set` con umbralTemp (-40 a 80), histeresis (0 a 10) e intervaloMs (2000 a 60000); se valida todo antes de aplicar y si algo es inválido se rechaza el comando completo. `reporte` solo publica el estado.

Comandos del Nodo B (mismo formato y misma validación que A, tópicos `nodoB/...` y el común `todos/cmd`):
`extractor` con valor 0 o de 40 a 100 (pasa a modo manual). Un valor de 1 a 39 se rechaza porque el ventilador no gira de forma confiable por debajo del mínimo: el nodo no acepta una orden que su hardware no puede cumplir. `modo` con valor auto o manual. `set` con umbralHum (20 a 95), histeresisHum (0 a 20) e intervaloMs (2000 a 60000). `reporte` solo publica el estado.

El estado del Nodo B incluye un campo `motivo` que explica por qué gira el extractor: humedad, temperaturaA, preventivo, normal o manual.

Las variables de operación se guardan en flash con la librería Preferences.

## Broker Mosquitto

Mosquitto 2.1.2 instalado en la laptop como servicio de Windows. Arranca solo al encender (inicio automático) y corre con la cuenta LocalSystem. No necesita ninguna ventana abierta.

**Configuración.** Al final de C:\Program Files\mosquitto\mosquitto.conf se agregaron cinco líneas: `listener 1883`, `allow_anonymous false`, `password_file C:\mosquitto\passwd`, `log_dest file C:\mosquitto\mosquitto.log` y `log_type all`. Desde la versión 2.0 Mosquitto solo acepta conexiones de la propia laptop si no se declara un listener. Por eso la primera línea es obligatoria. Los archivos de usuarios y de registro están en C:\mosquitto porque esa ruta no tiene espacios y no requiere permisos de administrador para escribir.

**Usuarios.** Tres usuarios creados con mosquitto_passwd: nodoA, nodoB y plataforma. El usuario plataforma es para Node-RED y para las pruebas con mosquitto_sub y mosquitto_pub. Cada participante tiene sus propias credenciales y el registro muestra quién hizo cada conexión. El archivo passwd guarda solo hashes PBKDF2 con SHA-512 y no las claves. Las claves en texto plano de los nodos solo existen en secretos.h.

**Red de la laptop.** IP 192.168.0.5 en la red 192.168.0.0/24 con el router en 192.168.0.1. Esa IP va en MQTT_BROKER de secretos.h. La asigna el router por DHCP y puede cambiar. Falta reservarla en el router para la MAC del WiFi de la laptop. Las direcciones de hardware aleatorias de Windows quedan desactivadas para esa red porque una MAC aleatoria rompería la reserva. El día de la evaluación la laptop tendría otra IP en otra red. Es una razón más para llevar un router propio.

**Firewall de Windows.** La red WiFi se marcó como privada y se creó una regla de entrada que permite TCP en el puerto 1883 solo en el perfil privado. En una red pública el puerto queda cerrado. Sin la regla Windows descarta en silencio las conexiones al broker.

**Mensajes retenidos y persistencia.** La persistencia de Mosquitto no está activada. Al reiniciar el servicio se pierden los mensajes retenidos hasta que cada nodo se reconecta y vuelve a publicar su conexión y su estado. Se puede activar con `persistence true` y una ruta de almacenamiento. Se decide al armar Node-RED.

**Ids únicos.** Node-RED debe generar un id distinto para cada comando (por ejemplo con la hora en milisegundos y un contador). Un id repetido se trata como reintento y no se ejecuta.

**Caída del broker.** El Last Will lo publica el broker cuando desaparece un cliente. Si el que cae es el broker nadie publica "offline". Por eso Node-RED debe mostrar su propio indicador de conexión con el broker y no depender solo de los tópicos de conexión de los nodos.

**Problema encontrado y solución.** El servicio arrancaba y se detenía de inmediato sin mostrar error. A mano con `mosquitto -c mosquitto.conf -v` funcionaba bien. El registro del servicio mostró la causa: `Unable to open pwfile "C:\mosquitto\passwd"`. Esta versión de Mosquitto crea sus archivos con permisos solo para la cuenta que los crea. El passwd lo creó el usuario de Windows y el servicio corre como LocalSystem así que no podía leerlo. Se resolvió dando permiso de solo lectura a LocalSystem con `icacls C:\mosquitto\passwd /grant *S-1-5-18:R`. S-1-5-18 es el identificador fijo de LocalSystem y funciona en Windows de cualquier idioma. Cada vez que se use mosquitto_passwd hay que repetir ese comando porque la herramienta puede reescribir el archivo con los permisos restringidos. El registro tiene la misma restricción en sentido inverso: para leerlo hay que tomar posesión con takeown y darse permiso de lectura con icacls.

## Comportamiento del Nodo A

LED apagado significa normal. Parpadeo rápido (250 ms) significa alarma de temperatura en A o de humedad en B. Parpadeo lento (1 s) significa falla del sensor de A, del sensor de B o del enlace con B. La alarma tiene prioridad sobre la falla.

La alarma de temperatura usa histéresis. El sensor se declara en falla tras tres lecturas fallidas seguidas. Probado en la placa con los valores por defecto: la alarma se activa en 30.0 C y se desactiva en 29.0 C. El DHT22 entrega la temperatura en décimas así que el valor del monitor serie es el mismo que compara el código. Sin el Nodo B el LED vuelve al parpadeo lento al salir de la alarma porque falta el enlace con B.

El LED y el heartbeat los ejecuta el temporizador de salidas. Cualquier cambio de la alarma o del estado del sensor de A (por una lectura o por un comando set que cambia el umbral) dispara un heartbeat inmediato hacia B en menos de 50 ms.

**Comportamiento seguro:** si se pierde el enlace con B se descarta su última alarma y se muestra falla. Si el nodo está en manual y pierde la plataforma por 30 s regresa solo a automático. Watchdog del loop activado con enableLoopWDT (reinicia si el loop se traba unos 5 s).

## Comportamiento del Nodo B

**Regla general.** El Nodo B copia el diseño del Nodo A siempre que se puede. Reutiliza el código probado de MQTT, comandos, validación, Preferences, ESP-NOW, temporizador de salidas y modo seguro. Las únicas diferencias son las que impone su hardware. Así el diseño se explica una vez y se aplica en los dos nodos.

**Velocidad del extractor en modo automático** (misma idea que el LED de A: alarma antes que falla y falla antes que normal):
100 % si la humedad de B está en alarma. 60 % si A reporta temperatura alta con el enlace activo y su sensor funcionando. 40 % preventivo si no hay información confiable (sensor de B fallando, sensor de A fallando o enlace con A caído). 0 % si todo está normal. Las velocidades de 60 y 100 % son constantes en el código para no agrandar el alcance.

**Alarma de humedad.** Con histéresis igual que la de temperatura. Valores iniciales: umbralHum 70 % HR y histeresisHum 5 puntos. No se usa un valor normado para bodegas de azúcar: 70 % se eligió para que la demo funcione con la humedad normal de un cuarto en Guatemala y para poder activar la alarma echando el aliento sobre el sensor. Es configurable y el valor real lo definiría el equipo de calidad de la bodega.

**Comportamiento seguro.** Si se pierde el enlace con A se descarta su última alarma de temperatura (no se confía en un dato viejo) y el extractor pasa al 40 % preventivo porque sin el panel B no sabe si la temperatura está alta. Ventilar al mínimo cuesta poco y es la opción conservadora. El control de humedad sigue funcionando porque su sensor es local. Si falla el sensor de B también pasa al 40 % preventivo y reporta sensorOk falso para que A muestre la falla. Si se pierde la plataforma en modo manual vuelve a automático a los 30 s como A. En la demo el comportamiento seguro se ve: al desconectar A el extractor de B pasa al 40 % en unos 3.5 s.

**Temporizador de salidas.** El mismo patrón que A: el loop decide y el temporizador de 50 ms ejecuta. Aplica la velocidad del extractor con el impulso de arranque contado en ticks (10 ticks de 50 ms) y envía el heartbeat a A con la humedad, su alarma, el estado del sensor y el porcentaje del extractor. Solo escribe el PWM cuando cambia el valor.

**DHT22 del Nodo B.** Real y no simulado aunque el enunciado lo permitiría. Echarle el aliento y ver cómo se enciende el extractor en B y parpadea el LED en A es la mejor prueba visible de la interacción entre nodos. La humedad simulada queda solo como respaldo si falla algo el último día. Va en GPIO 4 como en A. Se conecta con jumpers hembra-macho porque la fila de 3V3 queda colgando fuera de la protoboard (ver Estado del trabajo).

## Limitaciones conocidas (declararlas en la presentación)

Si cae el router los nodos buscan la red en todos los canales y el enlace ESP-NOW se vuelve intermitente. Entra el comportamiento seguro.

Si solo cae el broker o la laptop el enlace entre nodos sigue funcionando. Pero mqtt.connect() de PubSubClient bloquea el loop hasta que vence el tiempo de espera de conexión TCP del core. Medido en la placa: 3002 ms por intento con un intento cada 5 s (el loop queda detenido el 60 % del tiempo). Antes del temporizador de salidas el LED se congelaba y el heartbeat hacia B podía retrasarse hasta unos 4 s. Eso supera el timeout de 3.5 s así que B habría declarado el enlace caído en cerca de la mitad de los intentos. Con el temporizador el LED y el heartbeat ya no dependen del loop. Durante el bloqueo siguen detenidos el procesamiento de comandos y las lecturas del sensor (se retrasan hasta 3 s) y la detección de pérdida de B puede retrasarse unos 3 s más de lo normal. La solución completa sería el cliente MQTT asíncrono de ESP-IDF (esp_mqtt). Se descartó por el riesgo de reescribir la parte MQTT cerca de la entrega y queda como mejora futura.

El broker no tiene lista de control de acceso (ACL). Cualquier usuario autenticado puede publicar y suscribirse a cualquier tópico. Por ejemplo nodoA podría publicar en nodoB/cmd. Una mejora sería un acl_file que limite cada nodo a sus propios tópicos.

Con log_type all el registro del broker guarda cada mensaje publicado y crece unos pocos megabytes por día. Sirve para desarrollo y demo. Después conviene dejar solo errores, advertencias y conexiones.

Con el broker detenido y la laptop encendida cada intento de conexión también tarda 3002 ms (medido). El firewall de Windows descarta en silencio las conexiones a un puerto donde ningún programa escucha en lugar de rechazarlas. Para el ESP32 un broker detenido es igual que una laptop apagada.

El regreso a automático por falta de plataforma ocurre entre 30 y 33 s después de perder el broker (medido). La decisión la toma el loop y puede quedar retrasada hasta 3 s por un intento de conexión en curso.

El primer intento de conexión MQTT ocurre unos 5 s después de encender el nodo porque el temporizador de reintentos arranca en cero y espera REINTENTO_MQTT_MS. Es un retraso de arranque aceptable.

Una lectura fallida del DHT22 tarda unos 73 ms (medido sin sensor conectado) porque la librería de Adafruit cuenta iteraciones y no tiempo real para su límite de espera. No afecta al LED ni al heartbeat.

## Elementos opcionales elegidos

Cifrado de ESP-NOW y autenticación en el broker con validación de rangos (implementados en ambos nodos). Reconexión automática (implementada). Registro histórico de lecturas y comandos en Node-RED con SQLite o CSV. Alarmas fuera de rango (ya existen en los nodos y solo falta mostrarlas en el dashboard). Actualización OTA con ArduinoOTA.

Prioridad por valor contra riesgo: primero el registro histórico en CSV (barato y vistoso) y la visualización de alarmas. La actualización OTA es la de menor prioridad porque agrega riesgo (credenciales, particiones, una placa sin firmware válido) y es la que menos aporta a lo que evalúa el enunciado. Si el 4 de octubre llega apretado se saca del alcance y se declara como trabajo futuro.

## Credenciales y repositorio

Las credenciales se separan del código en un archivo secretos.h que no se sube a GitHub (está en .gitignore). Contiene el SSID y la clave del WiFi, la IP del broker, las claves MQTT de ambos nodos (MQTT_PASS_NODO_A y MQTT_PASS_NODO_B) y las claves PMK y LMK de ESP-NOW. Es idéntico en ambos nodos igual que protocolo.h. Cada nodo toma su propia clave MQTT. Los usuarios MQTT, los tópicos y el puerto no son secretos y quedan en el .ino. En el repositorio se sube secretos_ejemplo.h con valores falsos como plantilla.

Si falta secretos.h el código no compila y muestra un mensaje que explica cómo crearlo (uso de __has_include). Si una clave de ESP-NOW no tiene exactamente 16 caracteres tampoco compila (static_assert).

Las claves PMK y LMK se regeneraron al crear secretos.h (16 caracteres aleatorios de solo letras y números). Las anteriores estuvieron escritas en protocolo.h y pudieron quedar en archivos compartidos o en el historial de Git. La MAC de cada nodo sí va en protocolo.h y se sube al repositorio porque solo tiene sentido dentro de la red local.

Limitación: cada nodo guarda en su flash la clave MQTT del otro nodo. Como no se usa cifrado de flash quien lea la memoria de un nodo obtiene todas las credenciales. Es aceptable para un prototipo y se declara en la presentación.

Repositorio en GitHub con carpetas nodo_a, nodo_b, node-red y docs. El PDF del enunciado no se publica por ser un documento de la empresa.

## Entorno

Código en C++ con el framework de Arduino en Arduino IDE 2.3.10. Librerías: PubSubClient 2.8, DHT sensor library 1.4.6 de Adafruit con Adafruit Unified Sensor 1.1.14 y ArduinoJson 7.4.2. El Nodo A versión 3 compila sin advertencias (Compiler warnings en "All") con los cores del ESP32 2.0.9 y 3.3.12. El core instalado en la laptop es el 3.3.12.

El Nodo A imprime en el monitor serie cuánto tarda cada intento de conexión MQTT y cada lectura del DHT22 (exitosa con su temperatura o fallida). Sirve como registro para diagnosticar la red y el sensor.

## Estado del trabajo

Nodo A versión 3 con LED y heartbeat en el temporizador de salidas. Compilado sin advertencias con los cores 2.0.9 y 3.3.12. Imprime cada lectura de temperatura y cada intento de conexión MQTT con su duración.

Mosquitto 2.1.2 funcionando como servicio con autenticación y firewall configurados.

**Pruebas del Nodo A en hardware real (30 de septiembre).** Todas con el nodo conectado al broker y observadas con mosquitto_sub y el monitor serie.

Estado periódico: llega en nodoA/estado cada 5 s con temperatura, alarma, patrón del LED, modo, las tres variables de operación y el estado del enlace con B. Conexión normal al broker en 27 ms.

Comando a un nodo (reporte): ack con ok verdadero y estado inmediato.

Variable remota (set umbralTemp 25 con 27.9 C en el cuarto): ack con ok verdadero y en el mismo segundo el estado con el umbral nuevo, la alarma activa y el LED en parpadeo rápido. El LED físico cambió al instante.

Validación (set umbralTemp 100): ack con ok falso y el mensaje del rango. El umbral siguió en 25. El nodo publicó su estado también después del rechazo.

Control manual (led on): LED encendido fijo en modo manual aunque la alarma seguía activa.

Comando a todos (modo auto por todos/cmd): ack de A y regreso al patrón automático.

Duplicado (mismo id dos veces): solo ack sin estado inmediato. Confirmado porque el estado siguiente trajo una lectura nueva del sensor y eso solo ocurre en el ciclo periódico.

Last Will: al desconectar el USB el broker publicó nodoA/conexion offline. Al reconectar el nodo publicó online y su estado.

Comportamiento seguro: con el nodo en manual se detuvo el broker. El LED siguió fijo y a los 30 a 33 s el nodo volvió solo a automático. Al volver el broker el nodo se reconectó sin intervención. Esa reconexión tardó 2791 ms probablemente porque el servicio aún estaba arrancando. Una conexión normal tarda 27 ms.

Con esto el Nodo A cumple en hardware real las restricciones 4 a 7 en lo que le corresponde por sí solo. La restricción 3 necesita al Nodo B.

**Pruebas del Nodo B (30 de septiembre).** Se leyó la MAC de su placa y se probó el extractor con un sketch mínimo de PWM. Los resultados están en la sección de Hardware.

**Nodo B versión 1.** Firmware escrito con las decisiones de la sección Comportamiento del Nodo B. Compilado sin advertencias con los cores 2.0.9 y 3.3.12.

**Prueba de los dos nodos juntos (1 de octubre, sin el DHT22 de B).** Observada con mosquitto_sub.

Enlace: ambos estados mostraron enlaceA y enlaceB verdaderos y perdidosA y perdidosB en 0 durante toda la prueba. B en 40 % preventivo por su sensor ausente y el LED de A en parpadeo lento por la falla del sensor de B recibida por ESP-NOW.

Interacción sin plataforma (restricción 3): con set umbralTemp 25 enviado solo a A el extractor de B pasó de 40 a 60 % con motivo temperaturaA. El estado de B llegó al broker antes que el estado de A que generó el comando. Explicación probable: A publica el ack y el estado seguidos y el segundo mensaje TCP espera el acuse del primero (algoritmo de Nagle) mientras el aviso por ESP-NOW ya llegó a B. Muestra que el enlace directo es más rápido que el camino por la plataforma.

Pérdida del enlace (restricción 7): al desconectar A el extractor de B pasó a 40 % preventivo con enlaceA falso y después llegó nodoA/conexion offline. El orden es el esperado: el timeout de ESP-NOW (3.5 s) es menor que el de MQTT (7.5 s).

Reconexión: al conectar A de nuevo B recuperó el enlace y volvió a 60 % antes de que llegara nodoA/conexion online. ESP-NOW arranca unos 2.5 s después de encender y MQTT a los 5 s. Confirma que el enlace entre nodos no depende de la plataforma. Tras un reinicio de A su secuencia vuelve a 0 y B lo tomó como nuevo inicio sin contar pérdidas falsas.

Observaciones: en una de las reconexiones A se reinició dos veces seguidas (uptimeS 5 dos veces y offline seguido de online). Lo más probable es el contacto del USB al enchufarlo. Si se repite sin tocar el cable hay que revisar la causa del reinicio en el monitor serie. Además se reutilizó el id c10 para regresar el umbral a 30. Se ejecutó solo porque A se había reiniciado y había olvidado el último id. Sin ese reinicio el nodo lo habría tratado como duplicado y habría reenviado el ack del comando anterior sin aplicar el cambio. El estado real habría mostrado el umbral sin cambiar (restricción 6) pero la plataforma debe generar un id único por comando.

**Comandos del Nodo B y desconexión de B (2 de octubre, sin el DHT22 de B).**

Comando a ambos nodos (restricción 4): reporte y modo auto por todos/cmd produjeron dos acks con el mismo id, uno de cada nodo. El orden de los acks cambió entre una prueba y otra (primero A y luego B, después al revés). Con un comando simultáneo no hay orden garantizado: la plataforma debe esperar las dos confirmaciones del mismo id sin suponer cuál llega primero. Un modo auto enviado a un nodo que ya estaba en auto responde ok sin efectos secundarios.

Extractor en manual: 80 % con modo y motivo manual estable en los estados siguientes. Apagado con 0 %. Encendido desde apagado al 50 %: el ventilador arranca con el impulso y desacelera hasta el 50 % sin detenerse (repetido tres veces). No hace falta alargar el impulso ni agregar una rampa. El estado reporta 50 % desde el primer momento aunque el PWM esté al 100 % durante los 500 ms del impulso: el estado informa la velocidad de operación y el impulso es la forma de llegar a ella.

Rechazos: extractor 25 rechazado por rango, extractor "50" (texto) rechazado por tipo y umbralHum 100 rechazado por rango. En los tres el estado siguiente mostró el valor anterior sin cambios. La validación exige el tipo de dato del contrato y no convierte texto en número.

Desconexión de B: al desconectar su USB A pasó a enlaceB falso unos 3.5 s después y luego llegó nodoB/conexion offline. Al reconectar A recuperó el enlace antes de que llegara nodoB/conexion online. perdidosB siguió en 0 tras el reinicio de B.

Con esto quedan probados en hardware los dos sentidos del enlace ESP-NOW, la detección de pérdida en ambos nodos, todos los comandos de los dos nodos, los comandos simultáneos y el comportamiento seguro. Falta solo el DHT22 del Nodo B.

**Montaje del Nodo B con el ESP32 al borde.** Las filas de pines de esta placa están a 1 pulgada. En una protoboard estándar eso deja un agujero libre de un solo lado y la placa no alcanza a cubrir dos protoboards juntas. Solución: la fila de VIN va insertada en la fila j con el cuerpo de la placa hacia afuera y la fila de 3V3 queda colgando fuera del borde. Cada pin de la fila de VIN tiene libres los agujeros f a i. Tres jumpers hembra-macho conectan 3V3 al riel inferior (3.3 V), GND al riel GND inferior y D4 a la columna DATA del DHT22. Los rieles superiores llevan 5 V para el extractor y nunca se unen con los de 3.3 V. Se apoya el lado colgante para que la placa quede horizontal y se revisa que ningún pin colgante toque un riel. Pull-up de 10 kohm entre la columna DATA y el riel de 3.3 V.

**Sistema completo (madrugada del 3 de octubre).** Con los dos DHT22 conectados el sistema quedó por primera vez en estado normal: humedad de B en 61 % (lecturas de 6 ms), extractor apagado con motivo normal, LED de A apagado y ninguna falla.

Prueba del aliento sobre el sensor de B (interacción de B hacia A sin plataforma): la humedad pasó de 61.7 a 95.9 % en una sola lectura. En ese mismo estado B mostró alarmaHum verdadero, extractor al 100 % y motivo humedad. A publicó fuera de su ciclo de 5 s un estado con alarmaHumB verdadero y el LED en parpadeo rápido. El aviso llegó por el heartbeat inmediato de ESP-NOW.

Histéresis: la humedad llegó a 100 % y bajó a 81.6, 73.1 y 67.1 %. A 67.1 % la alarma siguió activa aunque está por debajo del umbral de 70 % porque no había bajado de 70 - 5 = 65 %. A 64.4 % la alarma se apagó: extractor a 0 % con motivo normal en B y LED de A apagado otra vez. La alarma duró unos 55 s desde la primera lectura alta.

Nota: el DHT22 marcó 100 % durante unos 35 s por la condensación del aliento en la rejilla. Conviene no exagerar en la demo porque una exposición prolongada a humedad saturada puede desplazar temporalmente la lectura del sensor.

Con esto todas las restricciones del enunciado quedan demostradas en hardware real.

Pendiente: fijar el canal de 2.4 GHz en 6 en el router (quedó en Auto). Cambiar la contraseña de administración del router si es la de fábrica. Reservar la IP de la laptop en el router. Anotar la corriente de la etiqueta del ventilador. Revisar los contactos del ventilador en la protoboard. Documento formal del protocolo. Plataforma Node-RED. Opcionales y preparación de la presentación.

## Cronograma

25 al 27 de septiembre: compra de hardware y pruebas de cada sensor y actuador por separado. 28 al 30: comunicación MQTT y ESP-NOW entre nodos. 1 y 2 de octubre: lógica cruzada, comandos y variables. 3: dashboard en Node-RED. 4: robustez y opcionales. 5: documentación y ensayo de la presentación. 6: entrega.

Ajuste del 30 de septiembre según lo avanzado. 1 de octubre: Nodo B en la placa con el extractor y primera prueba de ESP-NOW entre nodos. 2: DHT22 del Nodo B, interacciones cruzadas completas y pruebas de todos los escenarios de falla. 3: dashboard en Node-RED. 4: opcionales por orden de prioridad. 5: documentación y ensayo. 6: entrega.
