#include <WiFi.h>
#include <WiFiMulti.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <time.h>
#include <sys/time.h>

#define SIMULAR 1

const char* REDE1  = "REDE_CASA";
const char* SENHA1 = "SENHA_CASA";
const char* REDE2  = "HOTSPOT_CELULAR";
const char* SENHA2 = "SENHA_HOTSPOT";
const char* AP_NOME  = "DrainWatch";
const char* AP_SENHA = "drainwatch123";
const unsigned long TEMPO_WIFI_MS = 15000;

const float ALTURA_BALDE = 45;
const float ELEVACAO     = 20;
const float NIVEL_MAX    = 40;
const float LIM_ATENCAO  = 50;
const float LIM_CRITICO  = 70;
const float ZONA_CEGA    = 20;

const int TRIG = 27, ECHO = 26;
const int RELE_PIN = 13;
const bool RELE_ATIVO_ALTO = true;

const float LIM_DESLIGA_BOMBA = 30;
const float NIVEL_MIN_BOMBA = 3;
const unsigned long TEMPO_MAX_BOMBA_MS = 120000;

const char* FUSO = "<-03>3";
const size_t LOG_MAX_BYTES = 16384;

const float ALTURA_SONDA = ALTURA_BALDE + ELEVACAO;

WebServer server(80);
WiFiMulti wifiMulti;

bool modoAP = false;
bool wifiOk = false;
unsigned long quedaMs = 0, tUltimaTentativa = 0, tUltimaMedida = 0;

float distanciaSim = ALTURA_SONDA;
float ultimaDistancia = ALTURA_SONDA;
int falhas = 0;
String statusLogado = "normal";

bool modoBombaManual = false, bombaManualLigada = false, bombaLigada = false;
bool bloqueioTempo = false;
String bombaBloqueio = "";
unsigned long bombaInicioMs = 0;

bool horaSincronizada() { return time(nullptr) > 1700000000; }

String agoraTexto() {
  if (!horaSincronizada()) return "sem-hora";
  time_t t = time(nullptr);
  struct tm info;
  localtime_r(&t, &info);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &info);
  return String(buf);
}

void rotacionarLog() {
  File f = LittleFS.open("/log.txt", "r");
  if (!f) return;
  size_t tam = f.size();
  if (tam <= LOG_MAX_BYTES) { f.close(); return; }
  f.seek(tam / 2);
  f.readStringUntil('\n');
  String resto = f.readString();
  f.close();
  File g = LittleFS.open("/log.txt", "w");
  if (g) { g.print(resto); g.close(); }
}

void registrar(const char* nivel, const String& msg) {
  String linha = agoraTexto() + " | " + nivel + " | " + msg;
  Serial.println(linha);
  File f = LittleFS.open("/log.txt", "a");
  if (f) { f.println(linha); f.close(); }
  rotacionarLog();
}

float lerDistancia() {
#if SIMULAR
  return distanciaSim;
#else
  digitalWrite(TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long us = pulseIn(ECHO, HIGH, 30000);
  if (us == 0) return -1;
  float d = us * 0.0343 / 2.0;
  if (d < ZONA_CEGA) return -1;
  return d;
#endif
}

void medir() {
  float d = lerDistancia();
  if (d < 0) { falhas++; } else { ultimaDistancia = d; falhas = 0; }
}

float nivelCm() {
  float n = ALTURA_SONDA - ultimaDistancia;
  return n < 0 ? 0 : n;
}
float percentual() { return nivelCm() / NIVEL_MAX * 100.0; }

String statusAtual() {
  if (falhas >= 5) return "erro";
  float p = percentual();
  if (p >= LIM_CRITICO) return "critico";
  if (p >= LIM_ATENCAO) return "atencao";
  return "normal";
}

void verificarMudancaStatus() {
  String s = statusAtual();
  if (s == statusLogado) return;
  String pct = String(percentual(), 0) + "%";
  if (s == "erro")         registrar("ERRO", "Sensor sem leitura valida");
  else if (s == "critico") registrar("AVISO", "Nivel CRITICO: " + pct);
  else if (s == "atencao") registrar("AVISO", "Nivel em atencao: " + pct);
  else                     registrar("INFO", "Nivel normal: " + pct);
  statusLogado = s;
}

void acionarRele(bool ligar) {
  if (RELE_ATIVO_ALTO) {
    pinMode(RELE_PIN, OUTPUT);
    digitalWrite(RELE_PIN, ligar ? HIGH : LOW);
  } else if (ligar) {
    pinMode(RELE_PIN, OUTPUT);
    digitalWrite(RELE_PIN, LOW);
  } else {
    pinMode(RELE_PIN, INPUT);
  }
}

void atualizarBomba() {
  String s = statusAtual();
  bool querBase;
  if (modoBombaManual) {
    querBase = bombaManualLigada;
  } else {
    querBase = bombaLigada;
    if (s == "critico") querBase = true;
    else if (percentual() < LIM_DESLIGA_BOMBA) querBase = false;
  }
  if (!querBase) bloqueioTempo = false;

  bool quer = querBase;
  bombaBloqueio = "";
  if (quer && s == "erro")                    { quer = false; bombaBloqueio = "sensor"; }
  else if (quer && nivelCm() < NIVEL_MIN_BOMBA) { quer = false; bombaBloqueio = "sem_agua"; }
  else if (quer && bloqueioTempo)             { quer = false; bombaBloqueio = "tempo_maximo"; }
  else if (quer && bombaLigada && millis() - bombaInicioMs > TEMPO_MAX_BOMBA_MS) {
    quer = false;
    bloqueioTempo = true;
    bombaBloqueio = "tempo_maximo";
    registrar("AVISO", "Bomba desligada: tempo maximo continuo atingido");
  }

  if (quer != bombaLigada) {
    bombaLigada = quer;
    if (quer) bombaInicioMs = millis();
    acionarRele(quer);
    registrar("INFO", String("Bomba ") + (quer ? "LIGADA" : "DESLIGADA") + (modoBombaManual ? " (comando)" : " (automatico)"));
  }
}

void conectarRede() {
  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(REDE1, SENHA1);
  wifiMulti.addAP(REDE2, SENHA2);
  Serial.println("Procurando redes Wi-Fi...");
  unsigned long inicio = millis();
  while (wifiMulti.run() != WL_CONNECTED && millis() - inicio < TEMPO_WIFI_MS) delay(300);

  if (WiFi.status() == WL_CONNECTED) {
    modoAP = false;
    wifiOk = true;
    configTzTime(FUSO, "pool.ntp.org", "time.nist.gov");
    registrar("INFO", "Wi-Fi conectado: " + WiFi.SSID() + ", IP " + WiFi.localIP().toString());
  } else {
    modoAP = true;
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_NOME, AP_SENHA);
    registrar("AVISO", String("Sem rede conhecida. Ponto de acesso ") + AP_NOME + ", IP " + WiFi.softAPIP().toString());
  }
}

void monitorarWifi() {
  if (modoAP) return;
  bool ok = (WiFi.status() == WL_CONNECTED);
  if (!ok && wifiOk) {
    quedaMs = millis();
    registrar("AVISO", "Wi-Fi perdido. Tentando reconectar");
  } else if (ok && !wifiOk) {
    registrar("INFO", "Wi-Fi reconectado apos " + String((millis() - quedaMs) / 1000) + " s. IP " + WiFi.localIP().toString());
  }
  wifiOk = ok;
  if (!ok && millis() - tUltimaTentativa > 10000) {
    tUltimaTentativa = millis();
    wifiMulti.run();
  }
}

void enviarJson(int codigo, const String& corpo) {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(codigo, "application/json", corpo);
}

String jsonStatus() {
  String ip = modoAP ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String ssid = modoAP ? String(AP_NOME) : WiFi.SSID();
  String j = "{";
  j += "\"distancia_cm\":" + String(ultimaDistancia, 1);
  j += ",\"nivel_cm\":" + String(nivelCm(), 1);
  j += ",\"percentual\":" + String(percentual(), 1);
  j += ",\"status\":\"" + statusAtual() + "\"";
  j += ",\"bomba\":{\"modo\":\"" + String(modoBombaManual ? "manual" : "auto") + "\"";
  j += ",\"estado\":\"" + String(bombaLigada ? "ligada" : "desligada") + "\"";
  j += ",\"bloqueio\":\"" + bombaBloqueio + "\"";
  j += ",\"tempo_ligada_s\":" + String(bombaLigada ? (millis() - bombaInicioMs) / 1000 : 0) + "}";
  j += ",\"ip\":\"" + ip + "\"";
  j += ",\"mac\":\"" + WiFi.macAddress() + "\"";
  j += ",\"ssid\":\"" + ssid + "\"";
  j += ",\"rede\":\"" + String(modoAP ? "ponto_de_acesso" : "estacao") + "\"";
  j += ",\"porta\":80";
  j += ",\"wifi_rssi\":" + String(modoAP ? 0 : WiFi.RSSI());
  j += ",\"hora_sincronizada\":" + String(horaSincronizada() ? "true" : "false");
  j += ",\"hora\":\"" + agoraTexto() + "\"";
  j += ",\"uptime_s\":" + String(millis() / 1000) + "}";
  return j;
}

void rotaStatus() { enviarJson(200, jsonStatus()); }

void rotaBomba() {
  String e = server.arg("estado");
  if (e == "on")        { modoBombaManual = true;  bombaManualLigada = true; }
  else if (e == "off")  { modoBombaManual = true;  bombaManualLigada = false; }
  else if (e == "auto") { modoBombaManual = false; }
  else { enviarJson(400, "{\"erro\":\"use estado=on, off ou auto\"}"); return; }
  bloqueioTempo = false;
  registrar("INFO", "Comando recebido: bomba " + e);
  atualizarBomba();
  enviarJson(200, jsonStatus());
}

void rotaHora() {
  long t = server.arg("t").toInt();
  if (t < 1700000000) { enviarJson(400, "{\"erro\":\"hora invalida\"}"); return; }
  struct timeval tv = { (time_t)t, 0 };
  settimeofday(&tv, NULL);
  setenv("TZ", FUSO, 1);
  tzset();
  registrar("INFO", "Hora ajustada pelo cliente");
  enviarJson(200, "{\"ok\":true}");
}

void rotaLog() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  File f = LittleFS.open("/log.txt", "r");
  if (!f) { server.send(200, "text/plain; charset=utf-8", ""); return; }
  server.streamFile(f, "text/plain; charset=utf-8");
  f.close();
}

void rotaLogLimpar() {
  LittleFS.remove("/log.txt");
  registrar("INFO", "Log limpo por comando");
  enviarJson(200, "{\"ok\":true}");
}

#if SIMULAR
void rotaSimular() {
  distanciaSim = server.arg("d").toFloat();
  enviarJson(200, "{\"ok\":true}");
}
#endif

void setup() {
  Serial.begin(115200);
  setenv("TZ", FUSO, 1);
  tzset();
  if (!LittleFS.begin(true)) Serial.println("ERRO: falha ao montar o sistema de arquivos");

  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  acionarRele(false);

  registrar("INFO", "Sistema iniciado");
  float distCheio = ALTURA_SONDA - NIVEL_MAX;
  if (distCheio < ZONA_CEGA + 5) registrar("AVISO", "Sonda muito perto da agua no nivel maximo. Aumente ELEVACAO");

  conectarRede();

  server.on("/api/status", rotaStatus);
  server.on("/api/bomba", rotaBomba);
  server.on("/api/hora", rotaHora);
  server.on("/api/log", rotaLog);
  server.on("/api/log/limpar", rotaLogLimpar);
#if SIMULAR
  server.on("/api/simular", rotaSimular);
#endif
  server.onNotFound([]() { enviarJson(404, "{\"erro\":\"rota nao encontrada\"}"); });
  server.begin();
  registrar("INFO", "Servidor HTTP escutando na porta 80");
}

void loop() {
  server.handleClient();
  monitorarWifi();

  if (millis() - tUltimaMedida >= 200) {
    tUltimaMedida = millis();
    medir();
    verificarMudancaStatus();
    atualizarBomba();
  }
}
