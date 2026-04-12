/**
 * AulaSync - ESP32 + RFID RC522
 * Lee tarjetas RFID y envía el UID al servidor via HTTP POST
 * 
 * ══════════════════════════════════════════════════════════
 *   CONEXIÓN DE PINES ESP32 ↔ RC522 (MFRC522)
 * ══════════════════════════════════════════════════════════
 *   RC522 Pin    →    ESP32 Pin
 *   ─────────────────────────────
 *   SDA (SS)     →    GPIO 5
 *   SCK          →    GPIO 18
 *   MOSI         →    GPIO 23
 *   MISO         →    GPIO 19
 *   IRQ          →    (No conectar)
 *   GND          →    GND
 *   RST          →    GPIO 22
 *   3.3V         →    3.3V
 * ══════════════════════════════════════════════════════════
 * 
 * IMPORTANTE: El RC522 opera a 3.3V. NO conectar a 5V.
 */

#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>

// ── Pines RC522 ──────────────────────────────────────────
#define SS_PIN    5
#define RST_PIN   22

// ── Configuración WiFi ──────────────────────────────────
const char* WIFI_SSID     = "TU_RED_WIFI";
const char* WIFI_PASSWORD = "TU_PASSWORD_WIFI";

// ── Configuración del servidor ──────────────────────────
const char* SERVER_URL = "http://192.168.1.100:3001/api/nfc/lectura";
const char* DEVICE_KEY = "esp32-aulasync-device-key-2026";
const char* DEVICE_LOCATION = "porteria_superior";

// ── Antirrebote (ms) ────────────────────────────────────
const unsigned long DEBOUNCE_MS = 3000;
const unsigned long SYNC_INTERVAL_MS = 15000;
const size_t MAX_QUEUE_EVENTS = 25;
const uint8_t MAX_SYNC_RETRIES = 5;
const char* PREF_NAMESPACE = "aulasync";
const char* PREF_QUEUE_KEY = "nfcQueue";

enum SendResult {
  SEND_OK,
  SEND_RETRYABLE_ERROR,
  SEND_PERMANENT_ERROR
};

// ── Objetos globales ────────────────────────────────────
MFRC522 rfid(SS_PIN, RST_PIN);
Preferences preferences;
String ultimoUID = "";
unsigned long ultimaLectura = 0;
unsigned long ultimoIntentoSync = 0;
String colaPendiente[MAX_QUEUE_EVENTS];
size_t totalPendientes = 0;

// ══════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  randomSeed((uint32_t)micros());
  Serial.println("\n=== AulaSync ESP32 RFID ===");

  // Iniciar SPI y RC522
  SPI.begin();
  rfid.PCD_Init();
  delay(100);

  // Verificar RC522
  byte version = rfid.PCD_ReadRegister(rfid.VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("[ERROR] RC522 no detectado. Revisar conexiones.");
    while (true) { delay(1000); }
  }
  Serial.print("[OK] RC522 version: 0x");
  Serial.println(version, HEX);

  preferences.begin(PREF_NAMESPACE, false);
  cargarColaPendiente();

  // Conectar WiFi
  conectarWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    sincronizarPendientes();
  }

  Serial.println("[OK] Sistema listo. Esperando tarjetas...\n");
}

// ══════════════════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════════════════
void loop() {
  // Reconectar WiFi si se pierde
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WARN] WiFi desconectado. Reconectando...");
    conectarWiFi();
  }

  if (WiFi.status() == WL_CONNECTED && totalPendientes > 0 && (millis() - ultimoIntentoSync) >= SYNC_INTERVAL_MS) {
    sincronizarPendientes();
  }

  // Verificar si hay tarjeta presente
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    delay(100);
    return;
  }

  // Leer UID en formato hexadecimal
  String uid = obtenerUID();
  unsigned long ahora = millis();

  // Antirrebote: ignorar misma tarjeta dentro del tiempo configurado
  if (uid == ultimoUID && (ahora - ultimaLectura) < DEBOUNCE_MS) {
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
    return;
  }

  ultimoUID = uid;
  ultimaLectura = ahora;

  Serial.print("[NFC] UID leido: ");
  Serial.println(uid);

  // Enviar al servidor
  const String eventoId = generarEventoId(uid);
  SendResult resultado = enviarLectura(uid, eventoId);
  if (resultado == SEND_OK) {
    Serial.println("[NFC] Lectura enviada correctamente.");
  } else if (resultado == SEND_RETRYABLE_ERROR) {
    encolarLectura(uid, eventoId);
    Serial.println("[NFC] Lectura almacenada en cola offline.");
  } else {
    Serial.println("[NFC] Lectura rechazada por servidor (error permanente). No se encola.");
  }

  // Liberar tarjeta
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}

// ══════════════════════════════════════════════════════════
//  FUNCIONES
// ══════════════════════════════════════════════════════════

/**
 * Conecta a la red WiFi configurada
 */
void conectarWiFi() {
  Serial.print("[WIFI] Conectando a ");
  Serial.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int intentos = 0;
  while (WiFi.status() != WL_CONNECTED && intentos < 40) {
    delay(500);
    Serial.print(".");
    intentos++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("[WIFI] Conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[ERROR] No se pudo conectar al WiFi.");
  }
}

/**
 * Extrae el UID de la tarjeta como string hexadecimal (ej: "A1B2C3D4")
 */
String obtenerUID() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) {
      uid += "0";
    }
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

/**
 * Envía el UID al servidor AulaSync via HTTP POST
 * Devuelve si fue exitoso, fallo reintentable o fallo permanente
 */
SendResult enviarLectura(String uid, String eventoId) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] Sin conexion WiFi.");
    return SEND_RETRYABLE_ERROR;
  }

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY);
  http.setTimeout(5000);

  // JSON payload
  String payload = "{\"id_carnet\":\"" + uid + "\",\"ubicacion\":\"" + String(DEVICE_LOCATION) + "\",\"evento_id\":\"" + eventoId + "\"}";
  Serial.print("[HTTP] Enviando: ");
  Serial.println(payload);

  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    String response = http.getString();
    Serial.print("[HTTP] Respuesta (");
    Serial.print(httpCode);
    Serial.print("): ");
    Serial.println(response);
    http.end();
    if (httpCode >= 200 && httpCode < 300) {
      return SEND_OK;
    }

    // Los 4xx (excepto 408 y 429) se tratan como errores permanentes para este payload.
    if (httpCode >= 400 && httpCode < 500 && httpCode != 408 && httpCode != 429) {
      return SEND_PERMANENT_ERROR;
    }

    return SEND_RETRYABLE_ERROR;
  } else {
    Serial.print("[ERROR] HTTP fallo: ");
    Serial.println(http.errorToString(httpCode));
    http.end();
    return SEND_RETRYABLE_ERROR;
  }
}

String generarEventoId(const String& uid) {
  return String(DEVICE_LOCATION) + "-" + uid + "-" + String(millis()) + "-" + String(random(0xFFFF), HEX);
}

void cargarColaPendiente() {
  totalPendientes = 0;
  String rawQueue = preferences.getString(PREF_QUEUE_KEY, "");
  if (!rawQueue.length()) return;

  int start = 0;
  while (start < rawQueue.length() && totalPendientes < MAX_QUEUE_EVENTS) {
    int nextBreak = rawQueue.indexOf('\n', start);
    if (nextBreak < 0) nextBreak = rawQueue.length();

    String item = rawQueue.substring(start, nextBreak);
    item.trim();
    if (item.length()) {
      colaPendiente[totalPendientes++] = item;
    }
    start = nextBreak + 1;
  }

  Serial.print("[QUEUE] Lecturas pendientes cargadas: ");
  Serial.println(totalPendientes);
}

void guardarColaPendiente() {
  String rawQueue = "";
  for (size_t i = 0; i < totalPendientes; i++) {
    if (i > 0) rawQueue += '\n';
    rawQueue += colaPendiente[i];
  }
  preferences.putString(PREF_QUEUE_KEY, rawQueue);
}

bool existeEventoPendiente(const String& eventoId) {
  for (size_t i = 0; i < totalPendientes; i++) {
    if (colaPendiente[i].startsWith(eventoId + "|")) {
      return true;
    }
  }
  return false;
}

void eliminarPendienteEnIndice(size_t index) {
  if (index >= totalPendientes) return;
  for (size_t i = index; i + 1 < totalPendientes; i++) {
    colaPendiente[i] = colaPendiente[i + 1];
  }
  if (totalPendientes > 0) {
    colaPendiente[totalPendientes - 1] = "";
    totalPendientes--;
  }
  guardarColaPendiente();
}

void encolarLectura(const String& uid, const String& eventoId) {
  if (!eventoId.length() || existeEventoPendiente(eventoId)) return;

  const String item = construirItemPendiente(eventoId, uid, 0);
  if (totalPendientes >= MAX_QUEUE_EVENTS) {
    Serial.println("[QUEUE] Cola llena, se elimina el registro mas antiguo.");
    eliminarPendienteEnIndice(0);
  }

  colaPendiente[totalPendientes++] = item;
  guardarColaPendiente();

  Serial.print("[QUEUE] Lectura almacenada offline. Pendientes: ");
  Serial.println(totalPendientes);
}

String construirItemPendiente(const String& eventoId, const String& uid, uint8_t reintentos) {
  return eventoId + "|" + uid + "|" + String(reintentos);
}

bool parsearPendiente(const String& item, String& eventoId, String& uid, uint8_t& reintentos) {
  int firstSep = item.indexOf('|');
  if (firstSep <= 0) return false;

  int secondSep = item.indexOf('|', firstSep + 1);
  if (secondSep < 0) {
    // Compatibilidad con formato anterior: evento|uid
    eventoId = item.substring(0, firstSep);
    uid = item.substring(firstSep + 1);
    reintentos = 0;
  } else {
    eventoId = item.substring(0, firstSep);
    uid = item.substring(firstSep + 1, secondSep);
    String retryStr = item.substring(secondSep + 1);
    retryStr.trim();
    int parsedRetries = retryStr.toInt();
    reintentos = parsedRetries < 0 ? 0 : (uint8_t)parsedRetries;
  }

  eventoId.trim();
  uid.trim();
  return eventoId.length() && uid.length();
}

void actualizarPendienteEnIndice(size_t index, const String& eventoId, const String& uid, uint8_t reintentos) {
  if (index >= totalPendientes) return;
  colaPendiente[index] = construirItemPendiente(eventoId, uid, reintentos);
  guardarColaPendiente();
}

void sincronizarPendientes() {
  if (WiFi.status() != WL_CONNECTED || totalPendientes == 0) return;

  ultimoIntentoSync = millis();
  Serial.print("[QUEUE] Sincronizando lecturas pendientes: ");
  Serial.println(totalPendientes);

  size_t index = 0;
  while (index < totalPendientes) {
    String eventoId;
    String uid;
    uint8_t reintentos = 0;

    if (!parsearPendiente(colaPendiente[index], eventoId, uid, reintentos)) {
      eliminarPendienteEnIndice(index);
      continue;
    }

    String expectedPrefix = String(DEVICE_LOCATION) + "-";
    if (!eventoId.startsWith(expectedPrefix)) {
      Serial.print("[QUEUE] Evento descartado por ubicacion invalida en evento_id: ");
      Serial.println(eventoId);
      eliminarPendienteEnIndice(index);
      continue;
    }

    SendResult resultado = enviarLectura(uid, eventoId);
    if (resultado == SEND_OK) {
      Serial.print("[QUEUE] Lectura sincronizada: ");
      Serial.println(uid);
      eliminarPendienteEnIndice(index);
    } else if (resultado == SEND_PERMANENT_ERROR) {
      Serial.print("[QUEUE] Evento descartado por error permanente del servidor: ");
      Serial.println(eventoId);
      eliminarPendienteEnIndice(index);
    } else {
      reintentos++;
      if (reintentos >= MAX_SYNC_RETRIES) {
        Serial.print("[QUEUE] Evento descartado por exceder reintentos: ");
        Serial.println(eventoId);
        eliminarPendienteEnIndice(index);
        continue;
      }

      actualizarPendienteEnIndice(index, eventoId, uid, reintentos);
      Serial.print("[QUEUE] No se pudo sincronizar. Reintento ");
      Serial.print(reintentos);
      Serial.print("/");
      Serial.println(MAX_SYNC_RETRIES);
      break;
    }
  }
}
