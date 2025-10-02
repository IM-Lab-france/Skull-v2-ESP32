#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>   // v7.x
#include <ESPmDNS.h>
#include <HTTPClient.h>    // Appels HTTP
#include <Preferences.h>   // Sauvegarde associations boutons
#include <ArduinoOTA.h>
#include <Update.h>
#include <ESP32HTTPUpdateServer.h>
#include "config.h"
#include "webinterface.h"  // Interface web séparée

// ========= Mappage GPIO (LOLIN S2 mini) =========
// Relais principal via optocoupleur + transistor
constexpr uint8_t PIN_RELAY       = 10;

// === Relais 8 (GPIO8) : état en fonction musique/boutons ===
// Règles:
// - Pas de musique -> ON
// - Appui sur un bouton -> OFF immédiat
// - Musique en cours -> OFF
// - Fin de musique -> ON
constexpr uint8_t PIN_RELAY_BOUTON = 8;

// 5 boutons en INPUT_PULLUP (contact -> GND) — éviter GPIO0 (strap)
constexpr uint8_t BTN_PINS[5]     = {1, 2, 4, 5, 7};

// LED bleue intégrée (S2 mini) sur IO15, souvent active LOW
constexpr int LED_PIN = 15;
bool hasLed = true;

// Configuration HTTP
const char* HTTP_ENQUEUE_URL  = "http://192.168.1.116:5050/api/enqueue";
const char* HTTP_SESSIONS_URL = "http://192.168.1.116:5050/api/sessions";

// ========= App / Drivers =========
WebServer server(80);
bool relayOn = false;
bool autoRelayEnabled = true; // Contrôle automatique du relais principal activé par défaut
Preferences prefs;

// Associations boutons -> chaînes de caractères
String buttonSessions[5] = {"", "", "", "", ""};

// Variables pour l'interrogation automatique de l'API
unsigned long lastApiCheck = 0;
const unsigned long API_CHECK_INTERVAL = 1000; // 1 seconde
String currentSession = "";

// Anti-rebond
struct BtnState {
  bool stable;
  bool lastRead;
  bool previousStable;
  unsigned long t;
};
BtnState btn[5];
const unsigned long DEBOUNCE_MS = 35;

// ======= Utils CORS =======
void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}
void handleOptions(){ addCORS(); server.send(204); }

// ======= Gestion des préférences =======
void loadButtonSessions() {
  prefs.begin("buttons", false);
  for (int i = 0; i < 5; i++) {
    String key = "btn" + String(i);
    buttonSessions[i] = prefs.getString(key.c_str(), "");
  }
  prefs.end();
}
void saveButtonSession(int buttonIndex, const String& session) {
  prefs.begin("buttons", false);
  String key = "btn" + String(buttonIndex);
  prefs.putString(key.c_str(), session);
  prefs.end();
  buttonSessions[buttonIndex] = session;
}

// ======= Contrôle relais =======
void setRelay(bool on){
  relayOn = on;
  digitalWrite(PIN_RELAY, on ? LOW : HIGH);
  if (hasLed) digitalWrite(LED_PIN, on ? LOW : HIGH); // active LOW
  Serial.print("[RELAY] PIN_RELAY (GPIO10) = ");
  Serial.println(on ? "ON" : "OFF");
}
void setRelayLED(bool on){
  // PORT8: on=true => niveau logique permettant d'alimenter le relais 8 (adapter LOW/HIGH selon câblage).
  digitalWrite(PIN_RELAY_BOUTON, on ? LOW : HIGH);
  Serial.print("[RELAY] PIN_RELAY_BOUTON (GPIO8) = ");
  Serial.println(on ? "ON" : "OFF");
}

// ======= Fonction d'interrogation de l'API sessions (musique) =======
void checkSessionsAPI() {
  if (WiFi.status() != WL_CONNECTED || !autoRelayEnabled) {
    return;
  }

  Serial.println("\n[API] Interrogation de " + String(HTTP_SESSIONS_URL));
  
  HTTPClient http;
  http.begin(HTTP_SESSIONS_URL);
  http.addHeader("Content-Type", "application/json");

  int httpResponseCode = http.GET();

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("[API] Reponse HTTP: " + String(httpResponseCode));
    Serial.println("[API] JSON recu: " + response);

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);

    if (!error) {
      // Vérifier "playlist" puis "current"
      JsonObject playlist = doc["playlist"];
      
      if (playlist) {
        JsonVariant current = playlist["current"];
        
        if (current.isNull()) {
          // Aucune musique en cours -> état repos
          Serial.println("[MUSIC] Statut: AUCUNE MUSIQUE EN COURS");
          Serial.println("[MUSIC] playlist.current = null");
          currentSession = "";
          Serial.println("[ACTION] Passage en mode REPOS:");
          setRelay(false);     // PIN_RELAY OFF
          setRelayLED(true);   // PIN_RELAY_BOUTON ON
        } else {
          // Musique en cours -> état actif
          String newSession = current["session"] | "";
          Serial.println("[MUSIC] Statut: MUSIQUE EN COURS");
          Serial.println("[MUSIC] Session: " + newSession);
          
          if (newSession != currentSession) {
            Serial.println("[MUSIC] Changement de session detecte: '" + currentSession + "' -> '" + newSession + "'");
            currentSession = newSession;
          }
          
          Serial.println("[ACTION] Passage en mode ACTIF:");
          setRelay(true);      // PIN_RELAY ON
          setRelayLED(false);  // PIN_RELAY_BOUTON OFF
        }
      } else {
        Serial.println("[API] ERREUR: Champ 'playlist' introuvable dans le JSON");
      }
    } else {
      Serial.println("[API] ERREUR parsing JSON: " + String(error.c_str()));
    }
  } else {
    Serial.println("[API] ERREUR HTTP: " + String(httpResponseCode));
  }

  http.end();
  Serial.println("[API] Fin de l'interrogation\n");
}

// ======= Fonction d'appel HTTP pour enqueue =======
void makeHTTPCall(int buttonIndex) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] WiFi not connected - HTTP call skipped");
    return;
  }
  
  if (buttonSessions[buttonIndex].isEmpty()) {
    Serial.println("[HTTP] Button " + String(buttonIndex) + " has no associated session");
    return;
  }

  Serial.println("[HTTP] Enqueue request for button " + String(buttonIndex));
  Serial.println("[HTTP] Session: " + buttonSessions[buttonIndex]);
  
  HTTPClient http;
  http.setTimeout(5000); // Timeout de 5 secondes
  http.begin(HTTP_ENQUEUE_URL);
  http.addHeader("Content-Type", "application/json");

  String payload = "{\"session\":\"" + buttonSessions[buttonIndex] + "\"}";
  Serial.println("[HTTP] Payload: " + payload);

  int httpResponseCode = http.POST(payload);

  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("[HTTP] Enqueue Response Code: " + String(httpResponseCode));
    Serial.println("[HTTP] Response: " + response);
  } else {
    Serial.println("[HTTP] Enqueue Error Code: " + String(httpResponseCode));
    
    // Détails des codes d'erreur courants
    switch(httpResponseCode) {
      case -1:  Serial.println("[HTTP] Connection failed"); break;
      case -2:  Serial.println("[HTTP] Send header failed"); break;
      case -3:  Serial.println("[HTTP] Send payload failed"); break;
      case -11: Serial.println("[HTTP] Read timeout - serveur trop lent ou injoignable"); break;
      default:  Serial.println("[HTTP] Unknown error"); break;
    }
  }

  http.end();
  Serial.println("[HTTP] Request completed\n");
}

// ======= Interface Web =======
void handleRoot() {
  addCORS();
  server.send_P(200, "text/html", HTML_PAGE);
}

void handleButtonConfig() {
  addCORS();

  if (server.method() == HTTP_GET) {
    JsonDocument doc;
    auto arr = doc["sessions"].to<JsonArray>();
    for (int i = 0; i < 5; i++) arr.add(buttonSessions[i]);
    String response; serializeJson(doc, response);
    server.send(200, "application/json", response);

  } else if (server.method() == HTTP_POST) {
    if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"error\":\"no body\"}"); return; }
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (error) { server.send(400, "application/json", "{\"error\":\"bad json\"}"); return; }

    int buttonIndex = doc["button"] | -1;
    String session = doc["session"] | "";
    if (buttonIndex >= 0 && buttonIndex < 5) {
      saveButtonSession(buttonIndex, session);
      server.send(200, "application/json", "{\"success\":true}");
    } else {
      server.send(400, "application/json", "{\"error\":\"invalid button index\"}");
    }
  }
}

// ======= Auto-relay (relais principal) =======
void handleAutoRelay() {
  addCORS();
  if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"error\":\"no body\"}"); return; }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, server.arg("plain"));
  if (error) { server.send(400, "application/json", "{\"error\":\"bad json\"}"); return; }

  autoRelayEnabled = doc["enabled"] | true;

  JsonDocument response;
  response["autoRelay"] = autoRelayEnabled;
  response["success"] = true;

  String responseStr; serializeJson(response, responseStr);
  server.send(200, "application/json", responseStr);
}

// ======= Status =======
void handleStatus() {
  addCORS();
  JsonDocument doc;

  auto wifi = doc["wifi"].to<JsonObject>();
  wifi["ip"]   = WiFi.localIP().toString();
  wifi["rssi"] = WiFi.RSSI();

  doc["relay"] = relayOn ? 1 : 0;
  doc["autoRelay"] = autoRelayEnabled;
  doc["currentSession"] = currentSession;

  auto arr = doc["buttons"].to<JsonArray>();
  for (int i=0;i<5;i++) arr.add(btn[i].stable ? 1 : 0);

  String s; serializeJson(doc, s);
  server.send(200, "application/json", s);
}

// ======= API relais principal =======
void handleRelay(){
  addCORS();
  if(!server.hasArg("plain")){ server.send(400,"application/json","{\"error\":\"no body\"}"); return; }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, server.arg("plain"));
  if (e) { server.send(400,"application/json","{\"error\":\"bad json\"}"); return; }
  bool on = doc["on"] | false;

  // Si le contrôle automatique est activé, on le désactive temporairement pour permettre le contrôle manuel
  if (autoRelayEnabled) {
    Serial.println("Controle manuel du relais - desactivation temporaire du controle automatique");
    autoRelayEnabled = false;
  }

  setRelay(on);
  server.send(200,"application/json", String("{\"relay\":") + (relayOn?1:0) + "}");
}

// ======= Boutons =======
void setupButtons(){
  for(int i=0;i<5;i++){
    pinMode(BTN_PINS[i], INPUT_PULLUP);
    bool r = digitalRead(BTN_PINS[i]);
    btn[i] = { r, r, r, millis() };
  }
}

void updateButtons(){
  unsigned long now = millis();
  for(int i=0;i<5;i++){
    bool r = digitalRead(BTN_PINS[i]);
    if (r != btn[i].lastRead) {
      btn[i].lastRead = r;
      btn[i].t = now;
    }
    if (now - btn[i].t >= DEBOUNCE_MS) {
      btn[i].previousStable = btn[i].stable;
      btn[i].stable = btn[i].lastRead;

      // Détecter l'APPUI (INPUT_PULLUP: HIGH -> LOW)
      if (btn[i].previousStable == HIGH && btn[i].stable == LOW) {
        Serial.println("Button " + String(i) + " pressed - enqueue + activation relais + PORT8 OFF");
        setRelayLED(false);   // Dès l'appui -> PORT8 OFF
        //setRelay(true);       // PIN_RELAY ON immédiatement
        makeHTTPCall(i);
      }
    }
  }
}

// ======= Arduino =======
void setup() {
  // PIN_RELAY démarre ETEINT (pas de musique au démarrage)
  pinMode(PIN_RELAY, OUTPUT); 
  setRelay(false);
  
  if (hasLed) { pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH); } // OFF (active LOW)

  // PORT8 : ON par défaut lorsqu'il n'y a pas de musique
  pinMode(PIN_RELAY_BOUTON, OUTPUT);
  setRelayLED(true);

  setupButtons();

  Serial.begin(115200); // USB-CDC sur S2

  // Charger les associations boutons sauvegardées
  loadButtonSessions();

  // Wi-Fi + IP statique
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.config(local_IP, gateway, subnet, dns1);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status()!=WL_CONNECTED){ delay(300); }

  // mDNS
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
  }

  // API
  server.onNotFound([]{ addCORS(); server.send(404,"application/json","{\"error\":\"not found\"}"); });
  server.on("/", HTTP_GET, []{ handleRoot(); });
  server.on("/api/status",  HTTP_GET, []{ handleStatus(); });
  server.on("/api/relay",   HTTP_POST,[]{ handleRelay(); });
  server.on("/api/auto-relay", HTTP_POST, []{ handleAutoRelay(); });
  server.on("/api/button-config", HTTP_GET, []{ handleButtonConfig(); });
  server.on("/api/button-config", HTTP_POST, []{ handleButtonConfig(); });
  server.on("/api/relay",   HTTP_OPTIONS,[]{ handleOptions(); });
  server.on("/api/auto-relay", HTTP_OPTIONS, []{ handleOptions(); });
  server.on("/api/*",       HTTP_OPTIONS,[]{ handleOptions(); });
  server.begin();

  Serial.println("Setup completed - HTTP calls enabled on button press");
  Serial.println("Auto relay monitoring enabled - checking sessions API every second");
  Serial.print("Web interface available at: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();
  updateButtons();

  // Vérification périodique de l'API sessions
  unsigned long now = millis();
  if (now - lastApiCheck >= API_CHECK_INTERVAL) {
    lastApiCheck = now;
    checkSessionsAPI();
  }
}