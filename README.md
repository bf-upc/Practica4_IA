# 🏠 Monitor de Consumo Eléctrico Doméstico con IA

[![Platform](https://img.shields.io/badge/Platform-ESP32--S3-blue.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Framework](https://img.shields.io/badge/Framework-Arduino%20%7C%20ESP--IDF-green.svg)](https://github.com/espressif/arduino-esp32)
[![PlatformIO](https://img.shields.io/badge/PlatformIO-Professional-orange.svg)](https://platformio.org/)
[![License](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Sistema avanzado de monitoreo energético para el hogar con análisis en tiempo real, detección de anomalías, clasificación de electrodomésticos mediante IA y predicción de consumo. Desarrollado para ESP32-S3 utilizando arquitectura multitarea con FreeRTOS.

## 🚀 Características Principales

- **📊 Procesamiento en Tiempo Real**: Muestreo ADC a 2 kHz con buffer doble y procesamiento mediante FFT
- **🤖 Clasificación IA de Electrodomésticos**: Reconoce 5 tipos de cargas (Nevera, Lavadora, Microondas, Aire Acondicionado, Standby)
- **⚡ Análisis de Calidad Eléctrica**: Cálculo de THD (Distorsión Armónica Total) y Factor de Potencia
- **🚨 Detección de Anomalías**: Sobrecargas, picos de corriente y mala calidad de red
- **📈 Predicción de Consumo**: Modelo de regresión polinómica para pronosticar consumo diario
- **🌐 Dashboard Web**: Visualización en tiempo real vía WebSocket con actualizaciones 1s
- **💾 Almacenamiento en la Nube**: Integración con InfluxDB para métricas históricas
- **🔧 Arquitectura RTOS**: 6 tareas FreeRTOS optimizadas para cores duales del ESP32-S3

## 📋 Tabla de Contenidos

- [Arquitectura del Sistema](#arquitectura-del-sistema)
- [Hardware Requerido](#hardware-requerido)
- [Estructura del Proyecto](#estructura-del-proyecto)
- [Configuración Inicial](#configuración-inicial)
- [Instalación](#instalación)
- [Uso](#uso)
- [Interpretación de Datos](#interpretación-de-datos)
- [Solución de Problemas](#solución-de-problemas)
- [Próximas Mejoras](#próximas-mejoras)

## 🏗️ Arquitectura del Sistema

El sistema utiliza una arquitectura basada en FreeRTOS con tareas distribuidas entre los dos cores del ESP32-S3:

```
┌─────────────────────────────────────────────────────────────────┐
│                         CORE 1 (Tiempo Real)                    │
├─────────────────────────────────────────────────────────────────┤
│  [Task_ADC] ────buffer doble───> [Task_DSP]                    │
│   Prio: 4                          Prio: 3                      │
│   2 kHz sampling                   FFT, RMS, THD, IA            │
│                                      │││                        │
└──────────────────────────────────────┼┼┼────────────────────────┘
                                       │││
                    ┌──────────────────┼┼┼──────────────────┐
                    │                  │││                  │
                    ▼                  ▼▼                  ▼
┌─────────────────────────────────────────────────────────────────┐
│                    CORE 0 (Servicio/Red)                        │
├─────────────────┬─────────────────┬─────────────────┬───────────┤
│ [Task_Storage]  │ [Task_Anomalia] │ [Task_Display]  │[Watchdog] │
│   Prio: 2       │   Prio: 3       │   Prio: 1       │  Prio: 1  │
│ 60s agregación  │ Detección       │ WebSocket       │ Monitoreo │
│ InfluxDB        │ Sobrecarga/THD  │ Serial UI       │ de salud  │
└─────────────────┴─────────────────┴─────────────────┴───────────┘
```

## 🛠️ Hardware Requerido

| Componente | Especificación | Función |
|------------|---------------|---------|
| **Microcontrolador** | ESP32-S3-DevKitC-1 | Procesamiento principal |
| **Sensor de Corriente** | SCT-013 (100A/50mA) | Medición no invasiva |
| **Circuito Acondicionador** | Burden resistor 33Ω | Acondicionamiento de señal |
| **Conexión** | Wi-Fi 2.4 GHz | Transmisión de datos |

## 📁 Estructura del Proyecto

Este es un proyecto PlatformIO con la siguiente estructura:

```
monitor-consumo-esp32/
├── platformio.ini              # Configuración de PlatformIO
├── .gitignore                  # Archivos ignorados por Git
├── README.md                   # Este archivo
├── src/
│   └── main.cpp                # Código principal del sistema
└── include/                    # Archivos de cabecera
    └── config.h                # Configuración (NO incluido en repo)
```

**Nota:** El archivo `include/config.h` no está incluido en el repositorio por razones de seguridad. Debes crearlo siguiendo las instrucciones en [Configuración Inicial](#configuración-inicial).

## 🔧 Configuración Inicial

### ⚠️ IMPORTANTE: Archivo de Configuración

Este proyecto utiliza un archivo `include/config.h` para almacenar credenciales y configuraciones sensibles. **Este archivo está incluido en `.gitignore`** por seguridad, por lo que no se encuentra en el repositorio.

**Para que el proyecto funcione, debes crear tu propio archivo `include/config.h`** con el siguiente contenido:

```cpp
// include/config.h
#ifndef CONFIG_H
#define CONFIG_H

// =============================================================================
// CONFIGURACIÓN DE RED WI-FI
// =============================================================================
#define WIFI_SSID       "TuRedWiFi"           // Cambia por el nombre de tu red
#define WIFI_PASSWORD   "TuContraseñaWiFi"    // Cambia por tu contraseña

// =============================================================================
// CONFIGURACIÓN DE INFLUXDB
// =============================================================================
#define INFLUXDB_URL    "http://ip-de-tu-influxdb:8086/api/v2/write?org=tu-org&bucket=tu-bucket&precision=s"
#define INFLUXDB_TOKEN  "tu-token-de-influxdb"

// =============================================================================
// CONFIGURACIÓN DE WEBSOCKET
// =============================================================================
#define WEBSOCKET_PORT   81

#endif
```

### 📝 Pasos para configurar:

1. **Clona el repositorio** (el archivo `include/config.h` NO estará presente):
   ```bash
   git clone https://github.com/tu-usuario/monitor-consumo-esp32.git
   cd monitor-consumo-esp32
   ```

2. **Crea la carpeta `include` si no existe**:
   ```bash
   mkdir -p include
   ```

3. **Crea tu propio `config.h`**:
   ```bash
   touch include/config.h
   # o usa cualquier editor de texto
   ```

4. **Copia y pega la plantilla** de arriba en `include/config.h`, reemplazando los valores con tu configuración real.

5. **Verifica que Git ignore correctamente** tu archivo de configuración:
   ```bash
   git check-ignore -v include/config.h
   # Debería mostrar la regla que lo está ignorando
   ```

6. **Si necesitas compartir el proyecto con otros**, documenta en el README qué variables deben configurar (¡nunca compartas tus credenciales reales!).

## 📥 Instalación

### Prerrequisitos

1. **PlatformIO** (recomendado) o **Arduino IDE**
   - Para PlatformIO: Instala [Visual Studio Code](https://code.visualstudio.com/) y la extensión [PlatformIO IDE](https://platformio.org/install/ide?install=vscode)

2. **Bibliotecas necesarias** (definidas en `platformio.ini`):
   - `EmonLib` - Medición energética
   - `ArduinoJson` - Procesamiento JSON
   - `WebSockets` - Comunicación en tiempo real

### Instalación con PlatformIO (Recomendada)

1. **Clona el repositorio**:
   ```bash
   git clone https://github.com/tu-usuario/monitor-consumo-esp32.git
   cd monitor-consumo-esp32
   ```

2. **Crea tu archivo `include/config.h`** siguiendo los pasos de [Configuración Inicial](#configuración-inicial).

3. **Abre el proyecto con PlatformIO**:
   ```bash
   code .   # Si usas VSCode
   # O abre VSCode y selecciona File → Open Folder
   ```

4. **Compila el proyecto**:
   ```bash
   pio run
   ```

5. **Sube a la placa** (conecta tu ESP32-S3 vía USB):
   ```bash
   pio run --target upload
   ```

6. **Monitor serie** para ver la salida:
   ```bash
   pio device monitor
   ```

### Archivo `platformio.ini` de referencia

El proyecto debe tener un `platformio.ini` similar a este:

```ini
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
lib_deps = 
    openenergymonitor/EmonLib@^1.2.0
    bblanchon/ArduinoJson@^6.21.3
    links2004/WebSockets@^2.3.5
```

## 💻 Uso

### Monitor Serie

Una vez cargado el código, conecta el monitor serie a 115200 baudios. Verás:

```
=====================================================
  Monitor Consumo Electrico + IA — ESP32-S3
  FreeRTOS | EmonLib | FFT | TFLite | InfluxDB | WS
=====================================================

[SETUP] EmonLib OK
[SETUP] Conectando WiFi....
[SETUP] WiFi OK — IP: 192.168.1.100
[SETUP] WebSocket en puerto 81
[SETUP] Sistema en marcha.

[Task_ADC] Iniciada — 2 kHz ADC buffer doble
[Task_DSP] Iniciada — RMS + FFT + THD + TFLite
[Task_Storage] Iniciada — 60s + InfluxDB
[Task_Anomalia] Iniciada — umbral 15A / dI 8A / THD 15%
[Task_Display] Iniciada — 1s serie + WebSocket

[Task_DSP] I=2.35A P=540.5W FP=0.98 THD=3.2% [Lavadora]

+-------------------------------------------------+
| Monitor ESP32-S3          Uptime:     45 s      |
+-------------------------+-----------------------+
| Corriente RMS :  2.345 A | Tension   :   230 V |
| Potencia act  :  539.4 W | Pot. ap.  :  539.4 VA|
| Pot. reactiva :    0.0 VAR| FP        :  1.000  |
| THD           :    3.2 % |                       |
+-------------------------+-----------------------+
| Dispositivo: Lavadora                             |
+-------------------------------------------+-----+
| Estado: Normal                              |
+-------------------------------------------------+
```

### Dashboard Web

1. **Conecta el ESP32 a tu Wi-Fi** (verifica en el monitor serie la IP asignada)
2. **Abre un navegador web** y accede a: `http://[IP-DEL-ESP32]:81`
3. **Visualiza en tiempo real**:
   - Gráficas de corriente y potencia
   - Indicador de THD
   - Clasificación del electrodoméstico actual
   - Predicción de consumo
   - Alertas de anomalías

> **Nota:** Para visualizar el dashboard, necesitas un cliente HTML/JS que se conecte al WebSocket. Puedes crear un archivo HTML simple o usar herramientas como [WebSocket King](https://websocketking.com/) para probar.

### InfluxDB

Los datos se almacenan automáticamente cada 60 segundos en tu base de datos InfluxDB con el siguiente esquema:

```
Measurement: energia
Tags: sensor=sct013, dispositivo=[nombre]
Fields: irms, potencia_w, potencia_va, fp, thd, kwh
```

## 📊 Interpretación de Datos

### THD (Total Harmonic Distortion)
- **< 5%**: Buena calidad de red, cargas lineales
- **5-15%**: Aceptable, presencia de cargas electrónicas
- **> 15%**: Calidad de red deficiente, revisar fuentes switching

### Factor de Potencia (FP)
- **> 0.95**: Excelente, carga mayormente resistiva
- **0.80-0.95**: Aceptable, presencia de motores
- **< 0.80**: Mejorable, considerar compensación

### Clasificación de Dispositivos
El sistema identifica automáticamente:
- **Standby/Reposo**: < 20W, THD bajo
- **Nevera**: 80-200W, THD medio, ciclos intermitentes
- **Lavadora**: 300-800W, THD variable
- **Microondas**: 800-1500W, THD alto en operación
- **Aire Acondicionado**: 1500-3000W, FP bajo

## 🐛 Solución de Problemas

### El código no compila por falta de config.h
```bash
# Solución: Crea tu archivo include/config.h siguiendo la plantilla
fatal error: config.h: No such file or directory
```

### Wi-Fi no se conecta
- Verifica credenciales en `include/config.h`
- Asegúrate de estar en una red 2.4 GHz (ESP32 no soporta 5 GHz)
- Comprueba la intensidad de señal con `WiFi.RSSI()`

### No se reciben datos en WebSocket
- Verifica que el puerto 81 esté accesible
- Comprueba que el cliente web esté en la misma red
- Revisa el monitor serie: "WS clientes: X"

### Datos incorrectos de corriente
- Verifica la calibración en `CALIBRACION_EMON` (60.6 para SCT-013 100A)
- Confirma el burden resistor (33Ω para SCT-013 100A)
- Ajusta offset DC si es necesario en `vTask_ADC`

### Stack overflow en tareas
- El watchdog reporta el uso de stack cada 5 segundos
- Si alguna tarea muestra < 200 bytes libres, aumenta su tamaño en `xTaskCreatePinnedToCore`

## 🔜 Próximas Mejoras

- [ ] Implementar TFLite Micro real con modelo entrenado en TensorFlow
- [ ] Añadir almacenamiento en SPIFFS para configuración persistente
- [ ] Integrar MQTT como alternativa a WebSocket
- [ ] Añadir OTA (Over-The-Air) updates
- [ ] Implementar Deep Sleep para modo de bajo consumo
- [ ] Dashboard con autenticación básica
- [ ] Exportar datos a CSV desde la web
- [ ] Añadir soporte para múltiples sensores (3 fases)
- [ ] Documentación de la API REST embebida

## 📝 Notas Importantes

### Seguridad
- **NUNCA** subas tu archivo `include/config.h` al repositorio
- El archivo está en `.gitignore` para evitar commits accidentales
- Las credenciales están en el binario compilado y pueden ser extraídas
- Para aplicaciones críticas, considera usar un módulo de seguridad externo

### Rendimiento
- El sistema está optimizado para procesar a 2 kHz en Core 1
- No modifiques las prioridades de las tareas sin entender las implicaciones
- Las colas con timeout 0 evitan bloqueos en la cadena de tiempo real

### Compatibilidad
- Probado en ESP32-S3-DevKitC-1
- Debería funcionar en cualquier ESP32-S3 con ADC12
- Para ESP32 clásico, ajustar pines ADC y calibración

## 🤝 Contribuciones

Las contribuciones son bienvenidas. Por favor:

1. Fork el proyecto
2. Crea tu rama de características (`git checkout -b feature/AmazingFeature`)
3. Commit tus cambios (`git commit -m 'Add some AmazingFeature'`)
4. Push a la rama (`git push origin feature/AmazingFeature`)
5. Abre un Pull Request

## 📄 Licencia

Este proyecto está bajo la Licencia MIT. Ver el archivo `LICENSE` para más detalles.

---

**Desarrollado con ❤️ para el monitoreo energético del hogar**
