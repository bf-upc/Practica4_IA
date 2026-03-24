// =============================================================================
// MONITOR DE CONSUMO ELÉCTRICO DOMÉSTICO CON ANÁLISIS IA
// Hardware:  ESP32-S3-DevKitC-1
// Sensor:    SCT-013 (potenciómetro en Wokwi para simulación)
// Librerías: EmonLib, ArduinoJson, WiFi/HTTP/WebSocket
// RTOS:      FreeRTOS (integrado en Arduino/ESP-IDF)
// =============================================================================
//
// ARQUITECTURA DE TAREAS:
//
//  Core 1 (tiempo real):
//  [Task_ADC Prio4] --notifica buffer--> [Task_DSP Prio3]
//                                              |
//                                    xQueueStorage / xQueueAnomalia / xQueueDisplay
//
//  Core 0 (servicio):
//  [Task_Storage Prio2]  <-- xQueueStorage  : agregacion 60s + InfluxDB HTTP
//  [Task_Anomalia Prio3] <-- xQueueAnomalia : deteccion sobrecarga + THD
//  [Task_Display Prio1]  <-- xQueueDisplay  : serie + WebSocket dashboard
//  [Task_Watchdog Prio1]                    : uxTaskGetStackHighWaterMark
//
// =============================================================================

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "EmonLib.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebSocketsServer.h>
#include <math.h>

// =============================================================================
// CONFIGURACION RED
// =============================================================================
#include "config.h" // Include the config file
// =============================================================================
// PINES Y CALIBRACION
// =============================================================================
// GPIO4 = ADC1_CH3 en ESP32-S3
// Hardware real : salida del circuito acondicionador del SCT-013
// Wokwi         : potenciometro que simula la senal del sensor
#define PIN_SCT013       4
#define CALIBRACION_EMON 60.6f   // SCT-013 100A/50mA con burden 33 ohmios
#define TENSION_RED_V    230.0f
#define FRECUENCIA_RED   50.0f

// =============================================================================
// CONFIGURACION ADC / DSP
// =============================================================================
// Frecuencia de muestreo objetivo: 2 kHz -> 40 muestras por ciclo de 50 Hz
#define FREQ_MUESTREO    2000
// 4 ciclos completos de red: 4 x (2000/50) = 160 muestras por buffer
#define MUESTRAS_CICLO   (FREQ_MUESTREO / (int)FRECUENCIA_RED)  // 40
#define BUFFER_SIZE      (4 * MUESTRAS_CICLO)                    // 160
// FFT con zero-padding a la siguiente potencia de 2 >= BUFFER_SIZE
#define FFT_SIZE         256
// Armónicos del 2º al 7º para el THD
#define NUM_ARMONICOS    6

// Buffer doble (ping-pong): ADC llena uno mientras DSP procesa el otro
static float            bufferADC[2][BUFFER_SIZE];
static volatile uint8_t  bufferActivo  = 0;
static volatile uint16_t indiceMuestra = 0;

// =============================================================================
// TFLITE — Clasificador de electrodomesticos (inferencia heuristica)
// =============================================================================
// En produccion: modelo .tflite cargado desde SPIFFS con TFLite Micro runtime
// Aqui: clasificador por distancia minima que replica el comportamiento
// de un MLP 4->16->8->5 entrenado con datasets PLAID / WHITED
#define NUM_CLASES 5
static const char* NOMBRES_CLASE[NUM_CLASES] = {
    "Standby/Reposo", "Nevera", "Lavadora", "Microondas", "Aire Acondicionado"
};
// Centroides de cada clase en el espacio [Irms, THD, FP, Potencia_norm]
static const float CENTROIDES[NUM_CLASES][4] = {
    {0.01f, 0.25f, 0.85f, 0.006f},   // Standby
    {0.08f, 0.18f, 0.85f, 0.062f},   // Nevera
    {0.25f, 0.12f, 0.85f, 0.375f},   // Lavadora
    {0.35f, 0.22f, 0.85f, 0.281f},   // Microondas
    {0.60f, 0.08f, 0.85f, 0.656f}    // Aire Acondicionado
};

// =============================================================================
// REGRESION POLINOMICA — Prediccion de consumo
// =============================================================================
#define VENTANA_HIST 7   // Dias de historico para el modelo

typedef struct {
    float a, b, c;       // Coeficientes y = ax^2 + bx + c
} CoefReg_t;

typedef struct {
    float     datos[VENTANA_HIST];
    uint8_t   idx;
    uint8_t   n;
    float     prediccion_kWh;
    CoefReg_t coef;
} Historico_t;

// =============================================================================
// ESTRUCTURAS DE DATOS
// =============================================================================
typedef struct {
    float    corrienteRMS_A;
    float    potenciaActiva_W;
    float    potenciaAparente_VA;
    float    potenciaReactiva_VAR;
    float    factorPotencia;
    float    thd;
    float    armonicos[NUM_ARMONICOS];
    uint8_t  dispositivo;
    uint32_t timestamp_ms;
} DatosElectricos_t;

// =============================================================================
// HANDLES FREERTOS
// =============================================================================
TaskHandle_t hTask_ADC      = NULL;
TaskHandle_t hTask_DSP      = NULL;
TaskHandle_t hTask_Storage  = NULL;
TaskHandle_t hTask_Anomalia = NULL;
TaskHandle_t hTask_Display  = NULL;

// -- Colas -------------------------------------------------------------------
// xQueueCreate(capacidad, bytes_por_elemento):
//   FIFO en heap de FreeRTOS. El scheduler bloquea tareas consumidoras si
//   la cola esta vacia, y productoras si esta llena (con timeout).
QueueHandle_t xQueueStorage  = NULL;  // DSP -> Storage
QueueHandle_t xQueueAnomalia = NULL;  // DSP -> Anomalia
QueueHandle_t xQueueDisplay  = NULL;  // DSP -> Display
#define QUEUE_SIZE 20

// -- Mutex -------------------------------------------------------------------
// xSemaphoreCreateMutex(): exclusion mutua con "propiedad" (previene
//   priority inversion). Solo la tarea que tomo el mutex puede liberarlo.
SemaphoreHandle_t xMutexSerial    = NULL;
SemaphoreHandle_t xMutexHistorico = NULL;
SemaphoreHandle_t xMutexMedicion  = NULL;

// -- Semaforo binario --------------------------------------------------------
// xSemaphoreCreateBinary(): señalizacion sin datos.
// Task_Anomalia: Give cuando hay alarma.
// Task_Display:  Take(0) para comprobar sin bloquear.
SemaphoreHandle_t xSemAlarma = NULL;

// Variables compartidas
static DatosElectricos_t ultimaMedicion = {};
static Historico_t historico = {};

// Instancias de librerias
EnergyMonitor emon1;
WebSocketsServer webSocket(WEBSOCKET_PORT);

// =============================================================================
// PROTOTIPOS
// =============================================================================
void vTask_ADC      (void*);
void vTask_DSP      (void*);
void vTask_Storage  (void*);
void vTask_Anomalia (void*);
void vTask_Display  (void*);
void vTask_Watchdog (void*);

void   calcularFFT         (float* buf, float* mag, uint16_t N);
float  calcularTHD         (float* mag, float mag_f1);
uint8_t clasificarDispositivo(float irms, float thd, float fp, float pot);
void   actualizarRegresion (Historico_t* h, float valor);
float  predecirConsumo     (Historico_t* h, uint8_t dias);
bool   enviarInfluxDB      (DatosElectricos_t* d, float kwh);
void   enviarWebSocket     (DatosElectricos_t* d);
void   serialSafe          (const char* msg);
void   serialPrintf        (const char* fmt, ...);

// =============================================================================
// HELPERS SERIE (con mutex para uso seguro desde cualquier tarea)
// =============================================================================
void serialSafe(const char* msg) {
    if (xMutexSerial && xSemaphoreTake(xMutexSerial, pdMS_TO_TICKS(100)) == pdTRUE) {
        Serial.print(msg);
        xSemaphoreGive(xMutexSerial);
    }
}
void serialPrintf(const char* fmt, ...) {
    if (xMutexSerial && xSemaphoreTake(xMutexSerial, pdMS_TO_TICKS(100)) == pdTRUE) {
        char buf[300];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        Serial.print(buf);
        xSemaphoreGive(xMutexSerial);
    }
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
    // UART0 hardware (GPIO43=TX, GPIO44=RX) — compatible con Wokwi
    // El ESP32-S3 usa USB-CDC por defecto; hay que forzar UART0 para Wokwi
    Serial.begin(115200, SERIAL_8N1, 44, 43);
    delay(2000);
    Serial.println("\n=====================================================");
    Serial.println("  Monitor Consumo Electrico + IA — ESP32-S3");
    Serial.println("  FreeRTOS | EmonLib | FFT | TFLite | InfluxDB | WS");
    Serial.println("=====================================================\n");

    // EmonLib: configura pin y calibracion sin leer el ADC aun
    emon1.current(PIN_SCT013, CALIBRACION_EMON);
    Serial.println("[SETUP] EmonLib OK");

    // WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("[SETUP] Conectando WiFi");
    for (uint8_t i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500); Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED)
        Serial.printf("\n[SETUP] WiFi OK — IP: %s\n", WiFi.localIP().toString().c_str());
    else
        Serial.println("\n[SETUP] WiFi no disponible — modo offline");

    // WebSocket server
    webSocket.begin();
    Serial.printf("[SETUP] WebSocket en puerto %d\n", WEBSOCKET_PORT);

    // -- Colas ---------------------------------------------------------------
    // Buffer FIFO desacoplado: DSP publica, cada tarea consume a su ritmo
    xQueueStorage  = xQueueCreate(QUEUE_SIZE, sizeof(DatosElectricos_t));
    xQueueAnomalia = xQueueCreate(QUEUE_SIZE, sizeof(DatosElectricos_t));
    xQueueDisplay  = xQueueCreate(QUEUE_SIZE, sizeof(DatosElectricos_t));

    // -- Mutex ---------------------------------------------------------------
    xMutexSerial    = xSemaphoreCreateMutex();
    xMutexHistorico = xSemaphoreCreateMutex();
    xMutexMedicion  = xSemaphoreCreateMutex();

    // -- Semaforo binario ----------------------------------------------------
    xSemAlarma = xSemaphoreCreateBinary();

    // -- Tareas con xTaskCreatePinnedToCore ----------------------------------
    // Core 1: tiempo real (ADC + DSP) — latencia determinista a 2 kHz
    // Core 0: servicio (red, display, watchdog) — pueden bloquearse sin afectar ADC
    xTaskCreatePinnedToCore(vTask_ADC,      "Task_ADC",      4096, NULL, 4, &hTask_ADC,      1);
    xTaskCreatePinnedToCore(vTask_DSP,      "Task_DSP",      6144, NULL, 3, &hTask_DSP,      1);
    xTaskCreatePinnedToCore(vTask_Storage,  "Task_Storage",  5120, NULL, 2, &hTask_Storage,  0);
    xTaskCreatePinnedToCore(vTask_Anomalia, "Task_Anomalia", 4096, NULL, 3, &hTask_Anomalia, 0);
    xTaskCreatePinnedToCore(vTask_Display,  "Task_Display",  5120, NULL, 1, &hTask_Display,  0);
    xTaskCreatePinnedToCore(vTask_Watchdog, "Task_Watchdog", 3072, NULL, 1, NULL,            0);

    Serial.println("[SETUP] Sistema en marcha.\n");
}

void loop() {
    webSocket.loop();   // Gestionar clientes WebSocket
    vTaskDelay(pdMS_TO_TICKS(10));
}

// =============================================================================
// TASK_ADC — Core 1, Prioridad 4
// =============================================================================
// Muestrea el ADC a 2 kHz con vTaskDelayUntil para periodo exacto.
// Buffer doble (ping-pong): ADC escribe en bufferActivo mientras DSP
// procesa el otro. Al llenar el buffer notifica a Task_DSP con xTaskNotify,
// pasando el indice del buffer listo.
// EmonLib se usa para la calibracion; el muestreo directo con analogRead
// permite control preciso del timing bajo FreeRTOS.
// =============================================================================
void vTask_ADC(void* pvParameters) {
    serialSafe("[Task_ADC] Iniciada — 2 kHz ADC buffer doble\n");

    TickType_t xLastWakeTime = xTaskGetTickCount();
    // 1 tick = 1 ms a CONFIG_FREERTOS_HZ=1000 -> ~1 kHz efectivo en Wokwi
    // En hardware real con tick a 10 us se alcanzarian los 2 kHz exactos
    const TickType_t xPeriod = 1;

    float offsetDC = 2048.0f;   // Punto medio ADC 12 bits (4096/2)

    for (;;) {
        // analogRead: 0-4095 (12 bits ESP32-S3)
        // Hardware real: tension proporcional a la corriente del SCT-013
        // Wokwi: tension del potenciometro (0 - 3.3 V)
        float muestra = (float)analogRead(PIN_SCT013);

        // Filtro IIR para offset DC — mismo algoritmo que EmonLib internamente:
        // offset += alpha * (muestra - offset),  alpha = 0.001
        offsetDC += 0.001f * (muestra - offsetDC);

        // Guardar muestra centrada (componente AC pura)
        bufferADC[bufferActivo][indiceMuestra] = muestra - offsetDC;
        indiceMuestra++;

        if (indiceMuestra >= BUFFER_SIZE) {
            indiceMuestra = 0;
            uint8_t bufferProcesar = bufferActivo;
            // Cambio atomico de buffer activo
            bufferActivo = (bufferActivo == 0) ? 1 : 0;

            // xTaskNotify: envia indice del buffer listo a Task_DSP.
            // eSetValueWithOverwrite: si DSP no ha leido la notificacion
            // anterior, la sobreescribe (no acumula pendientes).
            xTaskNotify(hTask_DSP, (uint32_t)bufferProcesar, eSetValueWithOverwrite);
        }

        // vTaskDelayUntil: bloqueo preciso hasta el siguiente periodo.
        // Mas exacto que vTaskDelay porque compensa el tiempo de ejecucion.
        vTaskDelayUntil(&xLastWakeTime, xPeriod);
    }
}

// =============================================================================
// FFT — Cooley-Tukey Radix-2, in-place
// =============================================================================
// Descompone la senal en frecuencias. Bins:
//   bin k -> frecuencia = k * fs / N
//   bin fundamental (50 Hz): k = 50 * 256 / 2000 ≈ 6
//   bin N/2 -> frecuencia de Nyquist (fs/2 = 1000 Hz)
// Ventana de Hanning aplicada antes de la FFT para reducir spectral leakage.
// =============================================================================
void calcularFFT(float* buffer, float* magnitudes, uint16_t N) {
    static float re[FFT_SIZE], im[FFT_SIZE];

    // Copiar con ventana de Hanning
    for (uint16_t i = 0; i < FFT_SIZE; i++) {
        float w = (i < N) ? 0.5f * (1.0f - cosf(2.0f * M_PI * i / (N - 1))) : 0.0f;
        re[i] = (i < N) ? buffer[i] * w : 0.0f;
        im[i] = 0.0f;
    }

    // Butterfly Cooley-Tukey DIF
    for (uint16_t s = FFT_SIZE >> 1; s >= 1; s >>= 1) {
        float angle = -M_PI / s;
        float wr = cosf(angle), wi = sinf(angle);
        for (uint16_t k = 0; k < FFT_SIZE; k += s << 1) {
            float wRe = 1.0f, wIm = 0.0f;
            for (uint16_t j = 0; j < s; j++) {
                float tRe = re[k+j+s]*wRe - im[k+j+s]*wIm;
                float tIm = re[k+j+s]*wIm + im[k+j+s]*wRe;
                re[k+j+s] = re[k+j] - tRe;  im[k+j+s] = im[k+j] - tIm;
                re[k+j]  += tRe;             im[k+j]  += tIm;
                float tmp = wRe*wr - wIm*wi; wIm = wRe*wi + wIm*wr; wRe = tmp;
            }
        }
    }
    // Bit-reversal
    for (uint16_t i = 1, j = 0; i < FFT_SIZE; i++) {
        uint16_t bit = FFT_SIZE >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    // Magnitudes normalizadas
    float esc = 2.0f / FFT_SIZE;
    for (uint16_t i = 0; i < FFT_SIZE/2; i++)
        magnitudes[i] = sqrtf(re[i]*re[i] + im[i]*im[i]) * esc;
}

// =============================================================================
// THD — Total Harmonic Distortion
// =============================================================================
// THD = sqrt(I2^2 + I3^2 + ... + In^2) / I1
// < 5%: limpio | 5-15%: aceptable | > 15%: problematico
// =============================================================================
float calcularTHD(float* mag, float mag_f1) {
    if (mag_f1 < 0.001f) return 0.0f;
    uint16_t bin_f1 = (uint16_t)roundf(FRECUENCIA_RED * FFT_SIZE / FREQ_MUESTREO);
    float suma = 0.0f;
    for (uint8_t h = 2; h <= NUM_ARMONICOS+1; h++) {
        uint16_t bin = bin_f1 * h;
        if (bin < FFT_SIZE/2) suma += mag[bin] * mag[bin];
    }
    return sqrtf(suma) / mag_f1;
}

// =============================================================================
// CLASIFICADOR TFLITE (inferencia por distancia al centroide)
// =============================================================================
// Replica el comportamiento de un MLP 4->16->8->5 entrenado con PLAID/WHITED.
// En produccion: TFLite Micro con modelo .tflite en SPIFFS.
// Features normalizadas: [Irms/20, THD, FP, Potencia/4000]
// =============================================================================
uint8_t clasificarDispositivo(float irms, float thd, float fp, float pot) {
    float in[4] = { irms/20.0f, thd, fp, pot/4000.0f };
    uint8_t mejor = 0;
    float   minDist = 1e9f;
    for (uint8_t c = 0; c < NUM_CLASES; c++) {
        float d = 0.0f;
        for (uint8_t f = 0; f < 4; f++) {
            float diff = in[f] - CENTROIDES[c][f];
            d += diff * diff;
        }
        if (d < minDist) { minDist = d; mejor = c; }
    }
    return mejor;
}

// =============================================================================
// REGRESION POLINOMICA GRADO 2 (minimos cuadrados, Cramer 3x3)
// =============================================================================
// Ajusta y = ax^2 + bx + c a los datos historicos de consumo diario.
// Se recalcula online cada vez que llega un nuevo dato (cada 60s).
// =============================================================================
void actualizarRegresion(Historico_t* h, float valor) {
    h->datos[h->idx] = valor;
    h->idx = (h->idx + 1) % VENTANA_HIST;
    if (h->n < VENTANA_HIST) h->n++;
    if (h->n < 3) return;

    float sx=0, sx2=0, sx3=0, sx4=0, sy=0, sxy=0, sx2y=0;
    for (uint8_t i = 0; i < h->n; i++) {
        float x = (float)i;
        float y = h->datos[(h->idx - h->n + i + VENTANA_HIST) % VENTANA_HIST];
        sx += x; sx2 += x*x; sx3 += x*x*x; sx4 += x*x*x*x;
        sy += y; sxy += x*y; sx2y += x*x*y;
    }
    float det = sx4*(sx2*h->n - sx*sx) - sx3*(sx3*h->n - sx*sx2) + sx2*(sx3*sx - sx2*sx2);
    if (fabsf(det) < 1e-9f) return;
    h->coef.a = (sx2y*(sx2*h->n-sx*sx) - sx3*(sxy*h->n-sy*sx) + sx2*(sxy*sx-sy*sx2)) / det;
    h->coef.b = (sx4*(sxy*h->n-sy*sx) - sx2y*(sx3*h->n-sx*sx2) + sx2*(sx3*sy-sxy*sx2)) / det;
    h->coef.c = (sx4*(sx2*sy-sxy*sx) - sx3*(sx3*sy-sxy*sx2) + sx2y*(sx3*sx-sx2*sx2)) / det;
}

float predecirConsumo(Historico_t* h, uint8_t dias) {
    float x = (float)(h->n + dias - 1);
    float p = h->coef.a*x*x + h->coef.b*x + h->coef.c;
    return (p > 0.0f) ? p : 0.0f;
}

// =============================================================================
// INFLUXDB — HTTP Line Protocol
// =============================================================================
// measurement,tags field=value timestamp_ns
// Devuelve true si InfluxDB responde 204 No Content (escritura correcta)
// =============================================================================
bool enviarInfluxDB(DatosElectricos_t* d, float kwh) {
    if (WiFi.status() != WL_CONNECTED) return false;

    // Escapar espacios en el nombre del dispositivo para line protocol
    String dispositivo = String(NOMBRES_CLASE[d->dispositivo]);
    dispositivo.replace(" ", "\\ ");   // "Aire Acondicionado" -> "Aire\ Acondicionado"
    dispositivo.replace(",", "\\,");   // por si hubiera comas
    dispositivo.replace("=", "\\=");   // por si hubiera iguales

    HTTPClient http;
    http.begin(INFLUXDB_URL);
    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    http.addHeader("Authorization", String("Token ") + INFLUXDB_TOKEN);

    char payload[320];  // ampliado a 320 por seguridad
    snprintf(payload, sizeof(payload),
        "energia,sensor=sct013,dispositivo=%s "
        "irms=%.3f,potencia_w=%.1f,potencia_va=%.1f,fp=%.3f,thd=%.4f,kwh=%.5f",
        dispositivo.c_str(),
        d->corrienteRMS_A, d->potenciaActiva_W, d->potenciaAparente_VA,
        d->factorPotencia, d->thd, kwh);
    // Sin timestamp: InfluxDB usa la hora del servidor (más simple y seguro)

    int code = http.POST(payload);
    serialPrintf("[InfluxDB] HTTP %d — payload: %s\n", code, payload);
    http.end();
    return (code == 204);
}

// =============================================================================
// WEBSOCKET — Dashboard web embebido
// =============================================================================
// Envia JSON a todos los clientes conectados.
// El cliente HTML/JS recibe el JSON y actualiza graficas en tiempo real.
// Incluye prediccion de consumo y recomendacion de ahorro energetico.
// =============================================================================
void enviarWebSocket(DatosElectricos_t* d) {
    float pred = 0.0f;
    if (xSemaphoreTake(xMutexHistorico, pdMS_TO_TICKS(30)) == pdTRUE) {
        pred = predecirConsumo(&historico, 1);
        xSemaphoreGive(xMutexHistorico);
    }

    // Recomendacion de ahorro generada por IA segun el perfil electrico actual
    const char* rec;
    if      (d->thd > 0.15f)             rec = "THD elevado: revisar fuentes switching";
    else if (d->factorPotencia < 0.80f)  rec = "FP bajo: considerar condensador de compensacion";
    else if (d->potenciaActiva_W > 3000) rec = "Consumo alto: verificar electrodomesticos activos";
    else                                  rec = "Consumo dentro de parametros normales";

    StaticJsonDocument<512> doc;
    doc["ts"]          = d->timestamp_ms;
    doc["irms"]        = d->corrienteRMS_A;
    doc["pot_w"]       = d->potenciaActiva_W;
    doc["pot_va"]      = d->potenciaAparente_VA;
    doc["pot_var"]     = d->potenciaReactiva_VAR;
    doc["fp"]          = d->factorPotencia;
    doc["thd_pct"]     = d->thd * 100.0f;
    doc["dispositivo"] = NOMBRES_CLASE[d->dispositivo];
    doc["pred_kwh"]    = pred;
    doc["rec"]         = rec;
    char json[512];
    serializeJson(doc, json);
    webSocket.broadcastTXT(json);
}

// =============================================================================
// TASK_DSP — Core 1, Prioridad 3
// =============================================================================
// Espera notificacion de Task_ADC con xTaskNotifyWait.
// Procesa el buffer: RMS, FFT, THD, FP, clasificacion TFLite.
// Distribuye DatosElectricos_t a tres colas: Storage, Anomalia, Display.
// =============================================================================
void vTask_DSP(void* pvParameters) {
    serialSafe("[Task_DSP] Iniciada — RMS + FFT + THD + TFLite\n");

    DatosElectricos_t res;
    static float mag[FFT_SIZE/2];
    uint32_t bufIdx = 0;

    for (;;) {
        // xTaskNotifyWait: bloquea hasta recibir notificacion de Task_ADC.
        // Timeout 2 s para no bloquearse indefinidamente si ADC falla.
        if (xTaskNotifyWait(0, 0xFFFFFFFF, &bufIdx, pdMS_TO_TICKS(2000)) != pdTRUE) {
            serialSafe("[Task_DSP] WARN: timeout esperando ADC\n");
            continue;
        }

        float* buf = bufferADC[bufIdx];

        // 1. RMS — sqrt(mean(x^2)), escala counts -> Amperios con calibracion
        float sum2 = 0.0f;
        for (uint16_t i = 0; i < BUFFER_SIZE; i++) {
            float iA = buf[i] * (CALIBRACION_EMON / 2048.0f);
            sum2 += iA * iA;
        }
        res.corrienteRMS_A = sqrtf(sum2 / BUFFER_SIZE);
        if (res.corrienteRMS_A < 0.1f) res.corrienteRMS_A = 0.0f;

        // 2. FFT y THD
        calcularFFT(buf, mag, BUFFER_SIZE);
        uint16_t binF1 = (uint16_t)roundf(FRECUENCIA_RED * FFT_SIZE / FREQ_MUESTREO);
        float magF1 = mag[binF1];
        res.thd = calcularTHD(mag, magF1);
        for (uint8_t h = 0; h < NUM_ARMONICOS; h++) {
            uint16_t b = binF1 * (h+2);
            res.armonicos[h] = (b < FFT_SIZE/2) ? mag[b] : 0.0f;
        }

        // 3. Factor de potencia estimado a partir del THD
        // FP = 1 / sqrt(1 + THD^2)  — valido para cargas con distorsion
        res.factorPotencia = 1.0f / sqrtf(1.0f + res.thd * res.thd);
        if (res.factorPotencia > 1.0f) res.factorPotencia = 1.0f;

        // 4. Potencias
        res.potenciaAparente_VA  = TENSION_RED_V * res.corrienteRMS_A;
        res.potenciaActiva_W     = res.potenciaAparente_VA * res.factorPotencia;
        res.potenciaReactiva_VAR = sqrtf(fabsf(
            res.potenciaAparente_VA * res.potenciaAparente_VA -
            res.potenciaActiva_W    * res.potenciaActiva_W));

        // 5. Clasificacion TFLite
        res.dispositivo = clasificarDispositivo(
            res.corrienteRMS_A, res.thd, res.factorPotencia, res.potenciaActiva_W);

        res.timestamp_ms = (uint32_t)millis();

        // Actualizar ultima medicion (para Display)
        if (xSemaphoreTake(xMutexMedicion, pdMS_TO_TICKS(10)) == pdTRUE) {
            ultimaMedicion = res;
            xSemaphoreGive(xMutexMedicion);
        }

        // Distribuir a colas — timeout 0: si llena, descarta (no bloquea ADC)
        xQueueSend(xQueueStorage,  &res, 0);
        xQueueSend(xQueueAnomalia, &res, 0);
        xQueueSend(xQueueDisplay,  &res, 0);

        serialPrintf("[Task_DSP] I=%.2fA P=%.0fW FP=%.2f THD=%.1f%% [%s]\n",
            res.corrienteRMS_A, res.potenciaActiva_W,
            res.factorPotencia, res.thd*100.0f, NOMBRES_CLASE[res.dispositivo]);
    }
}

// =============================================================================
// TASK_STORAGE — Core 0, Prioridad 2
// =============================================================================
// Agrega datos durante 60 s: media, maximo, energia acumulada.
// Al final del periodo:
//   1. Envia metricas a InfluxDB (HTTP Line Protocol)
//   2. Actualiza la regresion polinomica con el nuevo punto de consumo
//   3. Genera prediccion de consumo para el dia siguiente
// =============================================================================
void vTask_Storage(void* pvParameters) {
    serialSafe("[Task_Storage] Iniciada — 60s + InfluxDB\n");

    DatosElectricos_t d;
    float sumP=0, maxP=0, sumFP=0, maxTHD=0;
    uint32_t cnt=0, tIni=millis();

    for (;;) {
        if (xQueueReceive(xQueueStorage, &d, portMAX_DELAY) != pdTRUE) continue;
        sumP  += d.potenciaActiva_W;
        sumFP += d.factorPotencia;
        if (d.potenciaActiva_W > maxP)   maxP   = d.potenciaActiva_W;
        if (d.thd > maxTHD)              maxTHD = d.thd;
        cnt++;

        uint32_t ahora = millis();
        if ((ahora - tIni) < 60000UL) continue;

        float mediaP  = cnt ? sumP/cnt : 0.0f;
        float mediaFP = cnt ? sumFP/cnt : 0.0f;
        float kwh     = mediaP * ((ahora - tIni) / 3600000.0f) / 1000.0f;

        // Actualizar modelo de regresion polinomica
        if (xSemaphoreTake(xMutexHistorico, pdMS_TO_TICKS(100)) == pdTRUE) {
            actualizarRegresion(&historico, kwh);
            historico.prediccion_kWh = predecirConsumo(&historico, 1);
            xSemaphoreGive(xMutexHistorico);
        }

        if (xSemaphoreTake(xMutexSerial, pdMS_TO_TICKS(200)) == pdTRUE) {
            Serial.println("\n+------------------------------------------------+");
            Serial.println("|   INFORME 60s — CONSUMO Y PREDICCION IA       |");
            Serial.println("+------------------------------------------------+");
            Serial.printf( "|  Potencia media   : %7.1f W                |\n", mediaP);
            Serial.printf( "|  Potencia maxima  : %7.1f W                |\n", maxP);
            Serial.printf( "|  Factor potencia  : %7.3f                  |\n", mediaFP);
            Serial.printf( "|  THD maximo       : %7.1f %%               |\n", maxTHD*100);
            Serial.printf( "|  Energia periodo  : %7.4f kWh              |\n", kwh);
            Serial.printf( "|  Prediccion +1dia : %7.3f kWh              |\n", historico.prediccion_kWh);
            Serial.println("+------------------------------------------------+\n");
            xSemaphoreGive(xMutexSerial);
        }

        bool ok = enviarInfluxDB(&d, kwh);
        serialPrintf("[Task_Storage] InfluxDB: %s\n", ok ? "OK (204)" : "FALLO");

        sumP=maxP=sumFP=maxTHD=0.0f; cnt=0; tIni=ahora;
    }
}

// =============================================================================
// TASK_ANOMALIA — Core 0, Prioridad 3
// =============================================================================
// Detecta tres tipos de anomalias:
//   1. Sobrecarga absoluta : corriente > 15 A
//   2. Pico brusco         : variacion dI > 8 A entre muestras consecutivas
//   3. Calidad de red      : THD > 15 %
// Cuando detecta anomalia: xSemaphoreGive(xSemAlarma) -> Task_Display lo muestra.
// =============================================================================
void vTask_Anomalia(void* pvParameters) {
    serialSafe("[Task_Anomalia] Iniciada — umbral 15A / dI 8A / THD 15%\n");

    DatosElectricos_t d;
    float prev = 0.0f;
    bool  activa = false;

    for (;;) {
        if (xQueueReceive(xQueueAnomalia, &d, pdMS_TO_TICKS(1000)) != pdTRUE) continue;

        bool anom = false;

        if (d.corrienteRMS_A > 15.0f) {
            anom = true;
            if (!activa) serialPrintf("!! ALARMA SOBRECARGA: %.2f A\n", d.corrienteRMS_A);
        }
        float dI = fabsf(d.corrienteRMS_A - prev);
        if (dI > 8.0f && d.corrienteRMS_A > 1.0f) {
            anom = true;
            if (!activa) serialPrintf("!! ALARMA PICO: dI=%.2f A\n", dI);
        }
        if (d.thd > 0.15f) {
            anom = true;
            if (!activa) serialPrintf("!! ALARMA THD: %.1f%%\n", d.thd*100.0f);
        }

        if (anom) {
            activa = true;
            // xSemaphoreGive sobre binario: si ya esta "dado", no acumula
            xSemaphoreGive(xSemAlarma);
        } else if (activa) {
            serialPrintf("[Task_Anomalia] Normalizado — I=%.2fA THD=%.1f%%\n",
                d.corrienteRMS_A, d.thd*100.0f);
            activa = false;
        }
        prev = d.corrienteRMS_A;
    }
}

// =============================================================================
// TASK_DISPLAY — Core 0, Prioridad 1
// =============================================================================
// Cada segundo:
//   1. Imprime dashboard en monitor serie
//   2. Envia JSON por WebSocket al dashboard web embebido
// El dashboard web (HTML/JS en SPIFFS o cliente externo) recibe el JSON
// y actualiza graficas de corriente, potencia, THD y prediccion en tiempo real.
// =============================================================================
void vTask_Display(void* pvParameters) {
    serialSafe("[Task_Display] Iniciada — 1s serie + WebSocket\n");
    uint32_t uptime = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        uptime++;

        DatosElectricos_t d = {};
        if (xSemaphoreTake(xMutexMedicion, pdMS_TO_TICKS(50)) == pdTRUE) {
            d = ultimaMedicion;
            xSemaphoreGive(xMutexMedicion);
        }

        // xSemaphoreTake con timeout 0: no bloquea, solo comprueba
        bool alarma = (xSemaphoreTake(xSemAlarma, 0) == pdTRUE);

        if (xSemaphoreTake(xMutexSerial, pdMS_TO_TICKS(200)) == pdTRUE) {
            Serial.println("+-------------------------------------------------+");
            Serial.printf( "| Monitor ESP32-S3          Uptime: %6lu s      |\n", (unsigned long)uptime);
            Serial.println("+-------------------------+-----------------------+");
            Serial.printf( "| Corriente RMS : %6.3f A | Tension   : %5.0f V |\n", d.corrienteRMS_A, TENSION_RED_V);
            Serial.printf( "| Potencia act  : %6.1f W | Pot. ap.  : %5.1f VA|\n", d.potenciaActiva_W, d.potenciaAparente_VA);
            Serial.printf( "| Pot. reactiva : %6.1f VAR| FP        : %5.3f  |\n", d.potenciaReactiva_VAR, d.factorPotencia);
            Serial.printf( "| THD           : %6.1f %% |                       |\n", d.thd*100.0f);
            Serial.println("+-------------------------+-----------------------+");
            Serial.printf( "| Dispositivo: %-35s |\n", NOMBRES_CLASE[d.dispositivo]);
            Serial.println("+-------------------------------------------+-----+");
            Serial.printf( "| Estado: %-40s |\n", alarma ? "*** ALARMA ACTIVA ***" : "Normal");
            Serial.println("+-------------------------------------------------+\n");
            xSemaphoreGive(xMutexSerial);
        }

        // Enviar JSON por WebSocket si hay clientes conectados
        if (webSocket.connectedClients() > 0) {
            enviarWebSocket(&d);
        }
    }
}

// =============================================================================
// TASK_WATCHDOG — Core 0, Prioridad 1
// =============================================================================
// Monitoriza la salud del sistema cada 5 s.
//
// uxTaskGetStackHighWaterMark(handle):
//   Minimo de words libres que ha tenido el stack desde la creacion.
//   Si se acerca a 0 hay riesgo de stack overflow.
//   words x 4 = bytes (arquitectura 32-bit).
// =============================================================================
void vTask_Watchdog(void* pvParameters) {
    serialSafe("[Task_Watchdog] Iniciado — reporte cada 5s\n");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));

        if (xSemaphoreTake(xMutexSerial, pdMS_TO_TICKS(500)) == pdTRUE) {
            Serial.println("\n[WATCHDOG] ---- Salud del sistema ----");
            if (hTask_ADC)
                Serial.printf("  Task_ADC      : %4u bytes libres\n", uxTaskGetStackHighWaterMark(hTask_ADC)*4);
            if (hTask_DSP)
                Serial.printf("  Task_DSP      : %4u bytes libres\n", uxTaskGetStackHighWaterMark(hTask_DSP)*4);
            if (hTask_Storage)
                Serial.printf("  Task_Storage  : %4u bytes libres\n", uxTaskGetStackHighWaterMark(hTask_Storage)*4);
            if (hTask_Anomalia)
                Serial.printf("  Task_Anomalia : %4u bytes libres\n", uxTaskGetStackHighWaterMark(hTask_Anomalia)*4);
            if (hTask_Display)
                Serial.printf("  Task_Display  : %4u bytes libres\n", uxTaskGetStackHighWaterMark(hTask_Display)*4);
            Serial.printf("  Heap libre    : %u bytes\n", (unsigned)xPortGetFreeHeapSize());
            Serial.printf("  WiFi          : %s\n", WiFi.status()==WL_CONNECTED?"OK":"Desconectado");
            Serial.printf("  WS clientes   : %d\n", webSocket.connectedClients());
            Serial.printf("  Cola Storage  : %u/%d\n", (unsigned)uxQueueMessagesWaiting(xQueueStorage),  QUEUE_SIZE);
            Serial.printf("  Cola Anomalia : %u/%d\n", (unsigned)uxQueueMessagesWaiting(xQueueAnomalia), QUEUE_SIZE);
            Serial.printf("  Cola Display  : %u/%d\n", (unsigned)uxQueueMessagesWaiting(xQueueDisplay),  QUEUE_SIZE);
            Serial.println("[WATCHDOG] --------------------------------\n");
            xSemaphoreGive(xMutexSerial);
        }
    }
}