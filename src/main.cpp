#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <vector>
#include <algorithm>

// -- WLAN-Zugangsdaten (bitte anpassen) --
const char* ssid     = "SCO-Net";
const char* password = "2Xxuarhusz7Z";

// -- Pinbelegung --
const int LED_PIN    = 33;  // LED an GPIO2
const int BUTTON_PIN = 32;  // Taster an GPIO4 (mit internem Pullup)

// ---------------------------------------------------
// Zustandsmaschine
enum TestState {
  IDLE,             // Wartet auf Start
  WAITING_TO_LIGHT, // nach Start, LED noch aus (Zufallszeit)
  LED_ON,           // LED leuchtet, warte auf Taster
  DONE              // Test fertig oder abgebrochen
};

TestState currentState = IDLE;

// Zeitvariablen
unsigned long waitToLightStart = 0; // wann Wartezeit begann
unsigned long waitToLightDelay = 0; // Zufall 5..15 s
unsigned long ledOnTimestamp   = 0; // wann LED an ging
unsigned long reactionTime     = 0; // Reaktionszeit

// Taster-Status
bool lastButtonState = true; // Pullup => normal HIGH

// Aktueller Spieler
String currentPlayerName = "";

// ---------------------------------------------------
// Leaderboard
struct Score {
  String name;
  unsigned long time; // ms
};

std::vector<Score> leaderboard;
const size_t MAX_LEADERBOARD_ENTRIES = 10;

void updateLeaderboard(const String& name, unsigned long timeMs) {
  Score s;
  s.name = name;
  s.time = timeMs;
  leaderboard.push_back(s);

  // Sortieren nach Zeit
  std::sort(leaderboard.begin(), leaderboard.end(),
            [](const Score &a, const Score &b){
              return a.time < b.time;
            });

  // Beschränken auf Top X
  if (leaderboard.size() > MAX_LEADERBOARD_ENTRIES) {
    leaderboard.resize(MAX_LEADERBOARD_ENTRIES);
  }
}

// ---------------------------------------------------
// WebSocket
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// Nachricht an alle WS-Clients
void broadcastMessage(const String &message) {
  ws.textAll(message);
}

// ---------------------------------------------------
// HTML-Seite
String indexPage() {
  String html = R"====(
<!DOCTYPE html>
<html lang="de">
<head>
  <meta charset="UTF-8" />
  <title>Reaktionstest</title>
  <style>
    body {
      margin: 0; 
      padding: 0; 
      font-family: Arial, sans-serif; 
      background: #f0f2f5;
      color: #333;
    }
    .container {
      max-width: 600px; 
      margin: 40px auto; 
      padding: 20px; 
      background: #fff; 
      border-radius: 8px; 
      box-shadow: 0 0 10px rgba(0,0,0,0.1);
    }
    h1 {
      text-align: center;
    }
    label {
      display: block; 
      margin-bottom: 8px; 
      font-weight: bold;
    }
    input[type="text"] {
      width: 100%; 
      padding: 10px; 
      margin-bottom: 10px; 
      border: 1px solid #ccc; 
      border-radius: 4px;
    }
    button {
      padding: 10px 16px; 
      border: none; 
      border-radius: 4px; 
      background: #007bff; 
      color: #fff; 
      cursor: pointer; 
      font-size: 16px;
    }
    button:hover {
      background: #0056b3;
    }
    .result {
      margin-top: 20px; 
      padding: 10px; 
      background: #e2e3e5; 
      border-radius: 4px;
    }
    .leaderboard {
      margin-top: 20px;
    }
    table {
      width: 100%; 
      border-collapse: collapse; 
      margin-top: 10px;
    }
    th, td {
      padding: 8px; 
      border-bottom: 1px solid #ddd;
      text-align: left;
    }
    th {
      background: #f7f7f7;
    }
    .error {
      color: #d9534f;
      font-weight: bold;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>Reaktionstest</h1>
    <div>
      <label for="nameInput">Dein Name:</label>
      <input type="text" id="nameInput" placeholder="Name eingeben" />
      <button id="startBtn">Start</button>
    </div>
    <div class="result" id="resultField">
      Noch kein Ergebnis.
    </div>
    <div class="leaderboard">
      <h2>Leaderboard (Top 10)</h2>
      <table id="leaderboardTable">
        <thead>
          <tr><th>Platz</th><th>Name</th><th>Zeit (ms)</th></tr>
        </thead>
        <tbody>
        </tbody>
      </table>
    </div>
  </div>
  <script>
    let ws;
    let nameInput       = document.getElementById('nameInput');
    let startBtn        = document.getElementById('startBtn');
    let resultField     = document.getElementById('resultField');
    let leaderboardBody = document.getElementById('leaderboardTable').querySelector('tbody');

    function initWebSocket() {
      ws = new WebSocket('ws://' + window.location.hostname + '/ws');
      ws.onmessage = function(event) {
        let msg = event.data;
        try {
          let data = JSON.parse(msg);

          if (data.type === "reactionResult") {
            // Reaktionszeit zurückbekommen
            resultField.innerHTML = "Deine Reaktionszeit: " + data.time + " ms";
          }
          else if (data.type === "leaderboard") {
            // Leaderboard aktualisieren
            updateLeaderboardTable(data.scores);
          }
          else if (data.type === "error") {
            // Fehlermeldung anzeigen
            resultField.innerHTML = '<span class="error">'+ data.message +'</span>';
          }

        } catch(e) {
          console.log("Received non-JSON message:", msg);
        }
      };
    }

    function updateLeaderboardTable(scores) {
      leaderboardBody.innerHTML = "";
      for(let i = 0; i < scores.length; i++) {
        let row = document.createElement('tr');
        let colPlace = document.createElement('td');
        let colName  = document.createElement('td');
        let colTime  = document.createElement('td');

        colPlace.textContent = (i+1);
        colName.textContent  = scores[i].name;
        colTime.textContent  = scores[i].time;

        row.appendChild(colPlace);
        row.appendChild(colName);
        row.appendChild(colTime);
        leaderboardBody.appendChild(row);
      }
    }

    startBtn.addEventListener('click', function() {
      let playerName = nameInput.value.trim();
      if (!playerName) {
        alert("Bitte Namen eingeben!");
        return;
      }
      // Start per WebSocket
      ws.send(JSON.stringify({
        type: "startTest",
        name: playerName
      }));
      resultField.innerHTML = "Warte auf das Startsignal...";
    });

    window.addEventListener('load', function() {
      initWebSocket();
    });
  </script>
</body>
</html>
)====";
  return html;
}

// ---------------------------------------------------
// WebSocket-Handler
void handleWebSocketMessage(void *arg, uint8_t *data, size_t len) {
  AwsFrameInfo *info = (AwsFrameInfo*)arg;
  if (info->opcode == WS_TEXT) {
    data[len] = 0; // String terminieren
    String msg = (char*)data;

    DynamicJsonDocument doc(200);
    DeserializationError error = deserializeJson(doc, msg);
    if (!error) {
      String type = doc["type"];
      if (type == "startTest") {
        // Spielername merken
        String playerName = doc["name"];
        currentPlayerName = playerName;

        // Test nur starten, wenn wir frei sind
        if (currentState == IDLE || currentState == DONE) {
          currentState = WAITING_TO_LIGHT;
          waitToLightDelay = random(5000, 15001); // 5..15 s
          waitToLightStart = millis();
          digitalWrite(LED_PIN, LOW);
          reactionTime = 0;
          ledOnTimestamp = 0;
        }
      }
    }
  }
}

void onEvent(AsyncWebSocket * server, AsyncWebSocketClient * client,
             AwsEventType type, void * arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      // Serial.println("WS client connected");
      break;
    case WS_EVT_DISCONNECT:
      // Serial.println("WS client disconnected");
      break;
    case WS_EVT_DATA:
      handleWebSocketMessage(arg, data, len);
      break;
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}

// ---------------------------------------------------
// Setup
void setup() {
  Serial.begin(9600);
  delay(100);

  // Zufall initialisieren
  randomSeed(analogRead(0));

  // Pins
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // WLAN
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Verbinde mit WLAN...");
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print(".");
  }
  Serial.println("\nWLAN verbunden! IP: " + WiFi.localIP().toString());

  // WebSocket
  ws.onEvent(onEvent);
  server.addHandler(&ws);

  // Route für HTML
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/html", indexPage());
  });

  // Server starten
  server.begin();
  Serial.println("Webserver gestartet.");
}

// ---------------------------------------------------
// Loop (Zustandsmaschine + Anti-Cheat)
void loop() {
  // Tasterzustand einlesen
  bool buttonState = digitalRead(BUTTON_PIN);

  switch (currentState) {
    case IDLE:
    case DONE:
      // Hier passiert nix, warten auf Start
      break;

    case WAITING_TO_LIGHT:
      // **ANTI-CHEAT**: Hat der Nutzer zu früh gedrückt?
      // = LED ist (noch) aus, aber Taster wurde gedrückt
      if (buttonState == LOW && lastButtonState == HIGH) {
        // => Test abbrechen / Schummeln erkannt
        currentState = DONE;

        // Sende ERROR an Browser
        {
          DynamicJsonDocument doc(128);
          doc["type"] = "error";
          doc["message"] = "Zu früh gedrückt! Test abgebrochen.";
          String out;
          serializeJson(doc, out);
          broadcastMessage(out);
        }
        break;
      }

      // Zeit abgelaufen => LED an
      if (millis() - waitToLightStart >= waitToLightDelay) {
        digitalWrite(LED_PIN, HIGH);
        ledOnTimestamp = millis();
        currentState = LED_ON;
      }
      break;

    case LED_ON:
      // Jetzt zählt die Zeit bis Tasterdruck
      if (buttonState == LOW && lastButtonState == HIGH) {
        reactionTime = millis() - ledOnTimestamp;
        digitalWrite(LED_PIN, LOW);

        // 1) Reaktionsergebnis schicken
        {
          DynamicJsonDocument doc(100);
          doc["type"] = "reactionResult";
          doc["time"] = reactionTime;
          String out;
          serializeJson(doc, out);
          broadcastMessage(out);
        }

        // 2) Leaderboard updaten
        updateLeaderboard(currentPlayerName, reactionTime);

        // 3) Neues Leaderboard an Browser
        {
          DynamicJsonDocument doc(512);
          doc["type"] = "leaderboard";
          JsonArray arr = doc.createNestedArray("scores");
          for (auto &sc : leaderboard) {
            JsonObject o = arr.createNestedObject();
            o["name"] = sc.name;
            o["time"] = sc.time;
          }
          String out;
          serializeJson(doc, out);
          broadcastMessage(out);
        }

        // Fertig
        currentState = DONE;
      }
      break;
  }

  lastButtonState = buttonState;
}