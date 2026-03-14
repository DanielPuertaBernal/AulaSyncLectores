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
 *   BUZZER (opcional, retroalimentación sonora)
 *   ─────────────────────────────
 *   Buzzer (+)   →    GPIO 2
 *   Buzzer (-)   →    GND
 * 
 *   LED indicador (opcional)
 *   ─────────────────────────────
 *   LED verde    →    GPIO 4  (lectura exitosa)
 *   LED rojo     →    GPIO 15 (error)
 * ══════════════════════════════════════════════════════════
 * 
 * IMPORTANTE: El RC522 opera a 3.3V. NO conectar a 5V.
 */

#include <SPI.h>
#include <MFRC522.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ── Pines RC522 ──────────────────────────────────────────
#define SS_PIN    5
#define RST_PIN   22

// ── Pines indicadores ────────────────────────────────────
#define BUZZER_PIN   2
#define LED_OK_PIN   4
#define LED_ERR_PIN  15

// ── Configuración WiFi ──────────────────────────────────
const char* WIFI_SSID     = "TU_RED_WIFI";
const char* WIFI_PASSWORD = "TU_PASSWORD_WIFI";

// ── Configuración del servidor ──────────────────────────
const char* SERVER_URL = "http://192.168.1.100:3001/api/nfc/lectura";
const char* DEVICE_KEY = "esp32-aulasync-device-key-2026";
const char* DEVICE_LOCATION = "porteria_superior";

// ── Antirrebote (ms) ────────────────────────────────────
const unsigned long DEBOUNCE_MS = 3000;

// ── Objetos globales ────────────────────────────────────
MFRC522 rfid(SS_PIN, RST_PIN);
String ultimoUID = "";
unsigned long ultimaLectura = 0;

// ══════════════════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== AulaSync ESP32 RFID ===");

  // Pines de salida
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_OK_PIN, OUTPUT);
  pinMode(LED_ERR_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_OK_PIN, LOW);
  digitalWrite(LED_ERR_PIN, LOW);

  // Iniciar SPI y RC522
  SPI.begin();
  rfid.PCD_Init();
  delay(100);

  // Verificar RC522
  byte version = rfid.PCD_ReadRegister(rfid.VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("[ERROR] RC522 no detectado. Revisar conexiones.");
    indicarError();
    while (true) { delay(1000); }
  }
  Serial.print("[OK] RC522 version: 0x");
  Serial.println(version, HEX);

  // Conectar WiFi
  conectarWiFi();

  indicarListo();
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
  bool exito = enviarLectura(uid);
  if (exito) {
    indicarExito();
  } else {
    indicarError();
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
    indicarError();
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
 * Returns true si el servidor respondió correctamente
 */
bool enviarLectura(String uid) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] Sin conexion WiFi.");
    return false;
  }

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY);
  http.setTimeout(5000);

  // JSON payload
  String payload = "{\"id_carnet\":\"" + uid + "\",\"ubicacion\":\"" + String(DEVICE_LOCATION) + "\"}";
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
    return (httpCode == 200 || httpCode == 201);
  } else {
    Serial.print("[ERROR] HTTP fallo: ");
    Serial.println(http.errorToString(httpCode));
    http.end();
    return false;
  }
}

/**
 * Señal de lectura exitosa: beep corto + LED verde
 */
void indicarExito() {
  digitalWrite(LED_OK_PIN, HIGH);
  tone(BUZZER_PIN, 2000, 150);
  delay(200);
  noTone(BUZZER_PIN);
  digitalWrite(LED_OK_PIN, LOW);
}

/**
 * Señal de error: 3 beeps rápidos + LED rojo
 */
void indicarError() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(LED_ERR_PIN, HIGH);
    tone(BUZZER_PIN, 500, 100);
    delay(150);
    noTone(BUZZER_PIN);
    digitalWrite(LED_ERR_PIN, LOW);
    delay(100);
  }
}

/**
 * Señal de sistema listo: 2 beeps ascendentes
 */
void indicarListo() {
  tone(BUZZER_PIN, 1000, 100);
  delay(150);
  tone(BUZZER_PIN, 2000, 100);
  delay(150);
  noTone(BUZZER_PIN);
}
