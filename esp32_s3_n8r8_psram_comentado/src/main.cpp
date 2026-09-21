/*
====================================================================
ESP32-S3-FreeNove - Servidor Web de GPIO / ADC
====================================================================

Este é o firmware principal do ESP32-S3.

O programa:
  - inicializa NVS/Preferences para guardar configurações;
  - monta o LittleFS, onde fica o index.html;
  - cria o Access Point Wi-Fi;
  - inicia o servidor HTTP;
  - configura individualmente os GPIOs como IN ou OUT;
  - lê entradas digitais e ADC;
  - controla saídas digitais;
  - disponibiliza APIs HTTP para o JavaScript da página.

PERSISTÊNCIA:
As configurações e estados dos GPIOs são armazenados no NVS.
O NVS deve ser aberto uma única vez, evitando chamadas repetidas
de Preferences.begin() durante a inicialização.

LITTLEFS:
O index.html fica em data/index.html e é gravado separadamente
com PlatformIO -> Upload Filesystem Image.

====================================================================
*/
/*
 * ================================================================
 * ESP32-S3-FreeNove
 * Servidor Web para I/O digital e ADC
 * ================================================================
 *
 * CORRECAO DA VERSAO ANTERIOR
 * ----------------------------
 * A versao anterior chamava Preferences.begin() repetidamente,
 * inclusive em modo somente-leitura, para cada GPIO.
 *
 * Quando a namespace "io" ainda nao existia, apareciam mensagens:
 *
 *   Preferences.cpp:50] begin(): nvs_open failed: NOT_FOUND
 *
 * Em seguida a placa entrava em:
 *
 *   rst:0x8 (TG1WDT_SYS_RST)
 *
 * Nesta versao:
 *   1) A namespace NVS e aberta UMA vez no setup().
 *   2) Ela e aberta em modo leitura/escrita para poder ser criada.
 *   3) save/load usam a mesma instancia Preferences.
 *   4) O programa continua funcionando mesmo se o NVS nao puder ser aberto.
 *   5) O projeto inicializa somente os recursos usados pelo servidor.
 *
 * A placa oficial do PlatformIO e:
 *   freenove_esp32_s3_wroom
 *
 * Ela corresponde a Freenove ESP32-S3 WROOM N8R8.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <stdarg.h>

// -----------------------------------------------------------------
// Configuracao do Access Point
// -----------------------------------------------------------------
const char *AP_SSID = "ESP32-S3-FreeNove";
const char *AP_PASS = "12345678";

/* Servidor HTTP na porta padrão 80. */
WebServer server(80);

// -----------------------------------------------------------------
// BUFFER JSON NA PSRAM
// -----------------------------------------------------------------
// A N8R8 possui 8 MB de PSRAM. O WebServer e a pilha Wi-Fi precisam da
// SRAM interna, por isso os documentos JSON maiores são montados na PSRAM.
// Se a PSRAM não estiver disponível, a classe usa a heap comum como fallback,
// mantendo o servidor funcional e registrando a situação no Monitor Serial.
class PsramJsonBuffer
{
public:
  explicit PsramJsonBuffer(size_t capacity) : capacity_(capacity)
  {
    data_ = static_cast<char *>(heap_caps_malloc(
        capacity_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    inPsram_ = data_ != nullptr;
    if (!data_)
      data_ = static_cast<char *>(malloc(capacity_));
    if (data_)
      data_[0] = '\0';
  }

  ~PsramJsonBuffer() { free(data_); }
  PsramJsonBuffer(const PsramJsonBuffer &) = delete;
  PsramJsonBuffer &operator=(const PsramJsonBuffer &) = delete;

  bool valid() const { return data_ != nullptr; }
  bool inPsram() const { return inPsram_; }
  const char *data() const { return data_ ? data_ : ""; }
  size_t length() const { return length_; }

  bool append(const char *text)
  {
    if (!text)
      return false;
    return appendFormat("%s", text);
  }

  bool appendFormat(const char *format, ...)
  {
    if (!data_ || length_ >= capacity_)
      return false;
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(data_ + length_, capacity_ - length_, format, args);
    va_end(args);
    if (written < 0 || static_cast<size_t>(written) >= capacity_ - length_)
    {
      data_[capacity_ - 1] = '\0';
      return false;
    }
    length_ += static_cast<size_t>(written);
    return true;
  }

private:
  char *data_ = nullptr;
  size_t capacity_ = 0;
  size_t length_ = 0;
  bool inPsram_ = false;
};

// Envia diretamente o bloco da PSRAM ao cliente, evitando convertê-lo para
// String (o que faria uma segunda cópia completa na SRAM interna).
void sendJsonBuffer(PsramJsonBuffer &json)
{
  if (!json.valid())
  {
    server.send(503, "application/json; charset=utf-8",
                "{\"ok\":false,\"error\":\"sem memoria para JSON\"}");
    return;
  }
  server.setContentLength(json.length());
  server.send(200, "application/json; charset=utf-8", "");
  server.client().write(reinterpret_cast<const uint8_t *>(json.data()), json.length());
}

// Escapa texto variável antes de inseri-lo em JSON.
String jsonEscape(String value)
{
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "\\r");
  value.replace("\t", "\\t");
  return value;
}

// -----------------------------------------------------------------
// AUTENTICACAO HTTP
// -----------------------------------------------------------------
//
// O navegador solicita usuario e senha antes de permitir acesso às
// páginas e às APIs. Altere estas duas constantes antes de colocar
// o ESP32 em uma rede acessível externamente.
//
// IMPORTANTE: esta é autenticação HTTP Basic. Ela protege as rotas,
// mas não criptografa o tráfego. Para acesso pela Internet, o ideal
// é usar uma VPN ou colocar HTTPS/TLS na frente do ESP32.
// -----------------------------------------------------------------
const char *ADMIN_USER = "admin";
const char *ADMIN_PASS = "esp32admin";

bool requireAuth()
{
  if (server.authenticate(ADMIN_USER, ADMIN_PASS))
  {
    return true;
  }

  server.requestAuthentication(BASIC_AUTH, "ESP32-S3-FreeNove");
  return false;
}

/* Objeto de acesso à memória NVS persistente. */
Preferences prefs;

// Indica se a namespace NVS foi aberta corretamente.
bool prefsReady = false;

/*
 * Configuração da rede Wi-Fi cliente (STA).
 * DHCP é o padrão. Quando static=true, os endereços salvos são usados.
 * A senha nunca é enviada pela API de status.
 */
String wifiSSID;
String wifiPassword;
bool wifiStatic = false;
IPAddress wifiIP(192, 168, 1, 200);
IPAddress wifiGateway(192, 168, 1, 1);
IPAddress wifiSubnet(255, 255, 255, 0);
IPAddress wifiDNS(8, 8, 8, 8);

// -----------------------------------------------------------------
// GPIOs que podem ser configurados individualmente pela pagina.
//
// Modos disponiveis:
//   IN  = entrada digital (INPUT)
//   OUT = saida digital (OUTPUT)
//   ADC = entrada analogica (somente GPIO 1..10)
//
// Foram excluidos GPIOs usados por USB, PSRAM, UART/console e
// funcoes especiais da placa para evitar conflitos de hardware.
// -----------------------------------------------------------------
const uint8_t CONFIG_PINS[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    11, 12, 13, 14, 15, 16, 17, 18,
    21, 38};
const size_t CONFIG_COUNT = sizeof(CONFIG_PINS) / sizeof(CONFIG_PINS[0]);

bool isConfigPin(uint8_t pin)
{
  for (size_t i = 0; i < CONFIG_COUNT; ++i)
  {
    if (CONFIG_PINS[i] == pin)
      return true;
  }
  return false;
}

bool isAdcCapable(uint8_t pin)
{
  return pin >= 1 && pin <= 10;
}

// mode: 0=IN, 1=OUT, 2=ADC
uint8_t loadPinMode(uint8_t pin)
{
  if (!prefsReady)
    return 0;
  char key[8];
  snprintf(key, sizeof(key), "m%u", pin);
  uint8_t mode = prefs.getUChar(key, 0);
  if (mode > 2 || (mode == 2 && !isAdcCapable(pin)))
    mode = 0;
  return mode;
}

void savePinMode(uint8_t pin, uint8_t mode)
{
  if (!prefsReady)
    return;
  char key[8];
  snprintf(key, sizeof(key), "m%u", pin);
  prefs.putUChar(key, mode);
}

bool loadOutputState(uint8_t pin)
{
  if (!prefsReady)
    return false;
  char key[8];
  snprintf(key, sizeof(key), "g%u", pin);
  return prefs.getBool(key, false);
}

void saveOutputState(uint8_t pin, bool state)
{
  if (!prefsReady)
    return;
  char key[8];
  snprintf(key, sizeof(key), "g%u", pin);
  prefs.putBool(key, state);
}

// Aplica o modo salvo de um GPIO.
void applyPinMode(uint8_t pin, uint8_t mode)
{
  if (mode == 1)
  {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, loadOutputState(pin) ? HIGH : LOW);
  }
  else
  {
    // INPUT simples: o nivel depende do circuito externo.
    // ADC tambem usa o pino como entrada.
    pinMode(pin, INPUT);
  }
}

// Configura todos os GPIOs com os modos salvos no NVS.
void restorePinModes()
{
  for (size_t i = 0; i < CONFIG_COUNT; ++i)
  {
    const uint8_t pin = CONFIG_PINS[i];
    applyPinMode(pin, loadPinMode(pin));
  }
}

String modeName(uint8_t mode)
{
  if (mode == 1)
    return "OUT";
  if (mode == 2)
    return "ADC";
  return "IN";
}

// -----------------------------------------------------------------
// Gera o JSON com estado dos GPIOs e valores ADC.
// -----------------------------------------------------------------
bool buildStatusJson(PsramJsonBuffer &json)
{
  const String ip = WiFi.status() == WL_CONNECTED
                        ? WiFi.localIP().toString()
                        : WiFi.softAPIP().toString();
  bool ok = json.appendFormat(
      "{\"ip\":\"%s\",\"mode\":\"%s\","
      "\"memory\":{\"psramFound\":%s,\"psramTotal\":%u,\"psramFree\":%u,"
      "\"internalFree\":%u,\"jsonInPsram\":%s},\"pins\":[",
      ip.c_str(), WiFi.status() == WL_CONNECTED ? "AP+STA" : "AP",
      psramFound() ? "true" : "false", ESP.getPsramSize(), ESP.getFreePsram(),
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL), json.inPsram() ? "true" : "false");

  for (size_t i = 0; i < CONFIG_COUNT; ++i)
  {
    if (i > 0)
      ok &= json.append(",");
    const uint8_t pin = CONFIG_PINS[i];
    const uint8_t mode = loadPinMode(pin);

    ok &= json.appendFormat("{\"pin\":%u,\"mode\":\"%s\"",
                            pin, modeName(mode).c_str());

    if (mode == 1)
    {
      ok &= json.appendFormat(",\"state\":%d", digitalRead(pin) ? 1 : 0);
    }
    else if (mode == 0)
    {
      ok &= json.appendFormat(",\"state\":%d", digitalRead(pin) ? 1 : 0);
    }
    else
    {
      const int value = analogRead(pin);
      const float voltage = (value / 4095.0f) * 3.3f;
      ok &= json.appendFormat(",\"value\":%d,\"voltage\":%.3f", value, voltage);
    }

    ok &= json.append("}");
  }

  ok &= json.append("]}");
  return ok;
}

// -----------------------------------------------------------------
// Pagina principal.
// -----------------------------------------------------------------
void handleRoot()
{
  File f = LittleFS.open("/index.html", "r");

  if (!f)
  {
    server.send(
        500,
        "text/plain; charset=utf-8",
        "index.html nao encontrado. Execute: Upload Filesystem Image");
    return;
  }

  server.streamFile(f, "text/html; charset=utf-8");
  f.close();
}

// -----------------------------------------------------------------
// API de estado.
// -----------------------------------------------------------------
void handleStatus()
{
  PsramJsonBuffer json(8192);
  if (!buildStatusJson(json))
  {
    server.send(507, "application/json; charset=utf-8",
                "{\"ok\":false,\"error\":\"buffer JSON insuficiente\"}");
    return;
  }
  sendJsonBuffer(json);
}

// -----------------------------------------------------------------
// API para ligar/desligar um GPIO.
// Exemplo:
//   /api/set?pin=12&state=1
// -----------------------------------------------------------------
void handleSet()
{
  if (!server.hasArg("pin") || !server.hasArg("state"))
  {
    server.send(400, "text/plain; charset=utf-8", "Parametros pin e state obrigatorios");
    return;
  }

  const int pin = server.arg("pin").toInt();
  const int state = server.arg("state").toInt();

  if (pin < 0 || pin > 48 || !isConfigPin(static_cast<uint8_t>(pin)))
  {
    server.send(400, "text/plain; charset=utf-8", "GPIO nao permitido");
    return;
  }

  const uint8_t gpio = static_cast<uint8_t>(pin);
  if (loadPinMode(gpio) != 1)
  {
    server.send(409, "text/plain; charset=utf-8", "GPIO nao esta configurado como OUT");
    return;
  }

  const bool value = state != 0;
  digitalWrite(gpio, value ? HIGH : LOW);
  saveOutputState(gpio, value);

  String response = "{\"ok\":true,\"pin\":";
  response += String(pin);
  response += ",\"state\":";
  response += value ? "1" : "0";
  response += "}";
  server.send(200, "application/json; charset=utf-8", response);
}

// -----------------------------------------------------------------
// API para configurar individualmente o modo de um GPIO.
// Exemplo:
//   /api/config?pin=12&mode=out
//   /api/config?pin=12&mode=in
//   /api/config?pin=5&mode=adc
// -----------------------------------------------------------------
void handleConfig()
{
  if (!server.hasArg("pin") || !server.hasArg("mode"))
  {
    server.send(400, "text/plain; charset=utf-8", "Parametros pin e mode obrigatorios");
    return;
  }

  const int pin = server.arg("pin").toInt();
  const String requested = server.arg("mode");

  if (pin < 0 || pin > 48 || !isConfigPin(static_cast<uint8_t>(pin)))
  {
    server.send(400, "text/plain; charset=utf-8", "GPIO nao permitido");
    return;
  }

  uint8_t mode;
  if (requested == "out")
    mode = 1;
  else if (requested == "adc")
    mode = 2;
  else if (requested == "in")
    mode = 0;
  else
  {
    server.send(400, "text/plain; charset=utf-8", "Modo invalido");
    return;
  }

  const uint8_t gpio = static_cast<uint8_t>(pin);
  if (mode == 2 && !isAdcCapable(gpio))
  {
    server.send(400, "text/plain; charset=utf-8", "Este GPIO nao possui ADC neste projeto");
    return;
  }

  Serial.printf("GPIO %u: alterando modo para %s\n", gpio, modeName(mode).c_str());
  savePinMode(gpio, mode);
  applyPinMode(gpio, mode);
  Serial.printf("GPIO %u: modo aplicado e salvo\n", gpio);

  String response = "{\"ok\":true,\"pin\":";
  response += String(pin);
  response += ",\"mode\":\"";
  response += modeName(mode);
  response += "\"}";
  server.send(200, "application/json; charset=utf-8", response);
}

// ============================================================================
// CONFIGURAÇÃO WI-FI SALVA NO NVS
// ============================================================================
//
// Lê do NVS os parâmetros usados pelo modo STA (cliente Wi-Fi).
// Se ainda não houver uma configuração salva, o ESP32 inicia somente no AP.
// ============================================================================
void loadWiFiConfig()
{
  if (!prefsReady)
  {
    wifiSSID = "";
    wifiPassword = "";
    wifiStatic = false;
    return;
  }

  wifiSSID = prefs.getString("wifi_ssid", "");
  wifiPassword = prefs.getString("wifi_pass", "");
  wifiStatic = prefs.getBool("wifi_static", false);

  wifiIP.fromString(prefs.getString("wifi_ip", "192.168.1.200"));
  wifiGateway.fromString(prefs.getString("wifi_gw", "192.168.1.1"));
  wifiSubnet.fromString(prefs.getString("wifi_mask", "255.255.255.0"));
  wifiDNS.fromString(prefs.getString("wifi_dns", "8.8.8.8"));
  Serial.print("Wi-Fi salvo: ");
  Serial.println(wifiSSID.length() ? wifiSSID : "(nenhum)");
}

// ============================================================================
// INICIALIZAÇÃO DA REDE
// ============================================================================
//
// AP  = Access Point criado pelo ESP32, usado para configuração.
// STA = Station, conexão do ESP32 ao roteador escolhido pelo usuário.
//
// O AP permanece ativo para que a placa continue acessível em
// 192.168.4.1 mesmo quando estiver conectada à rede local.
// ============================================================================
void startWiFi()
{
  WiFi.mode(WIFI_AP_STA);

  // Mantem o radio ativo para reduzir latencia e perda de resposta HTTP.
  // Em um servidor alimentado continuamente, estabilidade e mais importante
  // que a pequena economia obtida pelo modo sleep.
  WiFi.setSleep(false);

  if (!WiFi.softAP(AP_SSID, AP_PASS))
  {
    Serial.println("ERRO: falha ao iniciar Access Point.");
  }
  else
  {
    Serial.println("Modo AP iniciado.");
    Serial.print("SSID: ");
    Serial.println(AP_SSID);
    Serial.print("Senha: ");
    Serial.println(AP_PASS);
    Serial.print("IP: ");
    Serial.println(WiFi.softAPIP());
  }

  if (wifiSSID.length() == 0)
  {
    Serial.println("Nenhuma rede Wi-Fi salva. Somente AP ativo.");
    return;
  }

  Serial.print("Conectando a: ");
  Serial.println(wifiSSID);

  if (wifiStatic)
  {
    if (!WiFi.config(wifiIP, wifiGateway, wifiSubnet, wifiDNS))
    {
      Serial.println("AVISO: não foi possível aplicar IP fixo.");
    }
  }
  else
  {
    // DHCP: nenhuma configuração manual de IP é aplicada.
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  }

  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

  // Não bloqueia por longos períodos. Faz no máximo 10 tentativas.
  for (int attempt = 0; attempt < 10 && WiFi.status() != WL_CONNECTED; ++attempt)
  {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("Wi-Fi conectado.");
    Serial.print("IP local: ");
    Serial.println(WiFi.localIP());
  }
  else
  {
    Serial.println("Falha na conexão Wi-Fi. AP continua disponível.");
  }
}

// ============================================================================
// API: STATUS DO WI-FI
// ============================================================================
//
// Retorna para config.html:
// - conectado ou não;
// - SSID;
// - IP local;
// - IP do AP;
// - DHCP/IP fixo;
// - parâmetros de rede.
// A senha nunca é enviada ao navegador.
// ============================================================================
void handleWiFiStatus()
{
  PsramJsonBuffer json(2048);
  const String ssid = jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : wifiSSID);
  const String localIP = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  const bool ok = json.appendFormat(
      "{\"connected\":%s,\"ssid\":\"%s\",\"localIP\":\"%s\","
      "\"apIP\":\"%s\",\"dhcp\":%s,\"staticIP\":\"%s\","
      "\"gateway\":\"%s\",\"subnet\":\"%s\",\"dns\":\"%s\"}",
      WiFi.status() == WL_CONNECTED ? "true" : "false", ssid.c_str(),
      localIP.c_str(), WiFi.softAPIP().toString().c_str(),
      !wifiStatic ? "true" : "false", wifiIP.toString().c_str(),
      wifiGateway.toString().c_str(), wifiSubnet.toString().c_str(), wifiDNS.toString().c_str());
  if (!ok)
  {
    server.send(507, "application/json; charset=utf-8", "{\"error\":\"buffer insuficiente\"}");
    return;
  }
  sendJsonBuffer(json);
}

// ============================================================================
// API: VARREDURA DAS REDES
// ============================================================================
//
// Executa uma varredura Wi-Fi e devolve SSID, intensidade, canal e segurança.
// ============================================================================
void handleWiFiScan()
{
  int n = WiFi.scanNetworks(false, true);
  PsramJsonBuffer json(16384);
  bool ok = json.append("{\"networks\":[");
  for (int i = 0; i < n; ++i)
  {
    if (i > 0)
      ok &= json.append(",");
    const String ssid = jsonEscape(WiFi.SSID(i));
    ok &= json.appendFormat(
        "{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%d,\"secure\":%s}",
        ssid.c_str(), WiFi.RSSI(i), WiFi.channel(i),
        WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true");
  }
  ok &= json.append("]}");

  WiFi.scanDelete();
  if (!ok)
  {
    server.send(507, "application/json; charset=utf-8",
                "{\"error\":\"muitas redes para o buffer JSON\"}");
    return;
  }
  sendJsonBuffer(json);
}

// ============================================================================
// API: SALVAR CONFIGURAÇÃO WI-FI
// ============================================================================
//
// Recebe os parâmetros do formulário de config.html, valida os endereços,
// grava no NVS e tenta conectar à rede.
// ============================================================================
void handleWiFiSave()
{
  if (!server.hasArg("ssid") || !server.hasArg("password") || !server.hasArg("mode"))
  {
    server.send(400, "text/plain; charset=utf-8",
                "Parametros ssid, password e mode obrigatorios");
    return;
  }

  String ssid = server.arg("ssid");
  String password = server.arg("password");
  String mode = server.arg("mode");

  ssid.trim();

  if (ssid.length() == 0 || ssid.length() > 32 || password.length() > 63)
  {
    server.send(400, "text/plain; charset=utf-8", "SSID ou senha invalido");
    return;
  }

  bool useStatic = (mode == "static");

  IPAddress newIP, newGateway, newSubnet, newDNS;

  if (useStatic)
  {
    if (!server.hasArg("ip") || !server.hasArg("gateway") ||
        !server.hasArg("subnet") || !server.hasArg("dns") ||
        !newIP.fromString(server.arg("ip")) ||
        !newGateway.fromString(server.arg("gateway")) ||
        !newSubnet.fromString(server.arg("subnet")) ||
        !newDNS.fromString(server.arg("dns")))
    {
      server.send(400, "text/plain; charset=utf-8",
                  "Parametros de IP fixo invalidos");
      return;
    }
  }

  wifiSSID = ssid;
  wifiPassword = password;
  wifiStatic = useStatic;

  if (useStatic)
  {
    wifiIP = newIP;
    wifiGateway = newGateway;
    wifiSubnet = newSubnet;
    wifiDNS = newDNS;
  }

  if (prefsReady)
  {
    prefs.putString("wifi_ssid", wifiSSID);
    prefs.putString("wifi_pass", wifiPassword);
    prefs.putBool("wifi_static", wifiStatic);
    prefs.putString("wifi_ip", wifiIP.toString());
    prefs.putString("wifi_gw", wifiGateway.toString());
    prefs.putString("wifi_mask", wifiSubnet.toString());
    prefs.putString("wifi_dns", wifiDNS.toString());
  }

  WiFi.disconnect(false, false);
  delay(100);

  if (wifiStatic)
  {
    WiFi.config(wifiIP, wifiGateway, wifiSubnet, wifiDNS);
  }
  else
  {
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
  }

  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

  server.send(200, "application/json; charset=utf-8",
              "{\"ok\":true,\"message\":\"Configuracao salva. Conexao iniciada.\"}");
}

// -----------------------------------------------------------------
// SETUP
// -----------------------------------------------------------------
/*
 * setup() executa uma vez no boot.
 * Inicializa NVS, LittleFS, Wi-Fi, GPIOs e rotas HTTP.
 */
void setup()
{
  Serial.begin(115200);

  // Pequeno tempo para estabilizacao da porta serial.
  delay(300);

  Serial.println();
  Serial.println(
      "=== ESP32-S3-FreeNove GPIO + ADC SERVER ===");

  // Confirma em tempo de execução a memória esperada da variante N8R8.
  Serial.printf("Flash: %.2f MB\n", ESP.getFlashChipSize() / 1048576.0);
  Serial.printf("PSRAM: %.2f MB total / %.2f MB livre\n",
                ESP.getPsramSize() / 1048576.0,
                ESP.getFreePsram() / 1048576.0);
  Serial.printf("PSRAM detectada: %s\n", psramFound() ? "SIM" : "NAO");

  // ---------------------------------------------------------------
  // NVS / Preferences
  //
  // A versao anterior fazia begin() dezenas de vezes.
  // Agora abrimos somente uma vez.
  //
  // false = leitura + escrita.
  // Se a namespace "io" ainda nao existir, ela sera criada.
  // ---------------------------------------------------------------
  prefsReady = prefs.begin("io", false);

  if (prefsReady)
  {
    Serial.println("NVS: namespace 'io' aberta com sucesso.");
  }
  else
  {
    Serial.println(
        "AVISO: nao foi possivel abrir NVS. "
        "O projeto continuara sem persistencia.");
  }

  // ---------------------------------------------------------------
  // LittleFS
  // ---------------------------------------------------------------
  if (!LittleFS.begin(true))
  {
    Serial.println("ERRO: nao foi possivel montar LittleFS.");
  }
  else
  {
    Serial.println("LittleFS: OK.");
  }

  // ---------------------------------------------------------------
  // Restaura estados das saidas.
  // ---------------------------------------------------------------
  restorePinModes();

  // Carrega a configuração Wi-Fi salva.
  loadWiFiConfig();

  // Inicia AP de configuração + cliente Wi-Fi (STA).
  startWiFi();

  // ---------------------------------------------------------------
  // Rotas HTTP.
  // ---------------------------------------------------------------
  // Todas as páginas e APIs ficam protegidas pela mesma autenticação.
  // O navegador exibirá a janela de usuário/senha na primeira visita.
  server.on("/", HTTP_GET, []()
            { if (!requireAuth()) return; handleRoot(); });
  server.on("/index.html", HTTP_GET, []()
            { if (!requireAuth()) return; handleRoot(); });
  server.on("/config.html", HTTP_GET, []()
            {
    if (!requireAuth()) return;
    File f = LittleFS.open("/config.html", "r");
    if (!f) {
      server.send(500, "text/plain; charset=utf-8",
                  "config.html nao encontrado. Execute Upload Filesystem Image");
      return;
    }
    server.streamFile(f, "text/html; charset=utf-8");
    f.close(); });

  server.on("/api/status", HTTP_GET, []()
            { if (!requireAuth()) return; handleStatus(); });
  server.on("/api/set", HTTP_GET, []()
            { if (!requireAuth()) return; handleSet(); });
  server.on("/api/config", HTTP_GET, []()
            { if (!requireAuth()) return; handleConfig(); });
  server.on("/api/wifi/status", HTTP_GET, []()
            { if (!requireAuth()) return; handleWiFiStatus(); });
  server.on("/api/wifi/scan", HTTP_GET, []()
            { if (!requireAuth()) return; handleWiFiScan(); });
  server.on("/api/wifi/save", HTTP_GET, []()
            { if (!requireAuth()) return; handleWiFiSave(); });

  // Mostra no monitor serial qual URL gerou um 404. Isso evita a mensagem
  // generica "request handler not found" e ajuda no diagnostico.
  server.onNotFound([]()
                    {
    Serial.print("HTTP 404 - Rota nao encontrada: ");
    Serial.println(server.uri());
    server.send(404, "text/plain; charset=utf-8", "Rota nao encontrada: " + server.uri()); });

  server.begin();

  Serial.println("Servidor HTTP iniciado.");
  Serial.println("Setup concluido.");
}

// -----------------------------------------------------------------
// LOOP
// -----------------------------------------------------------------
/*
 * loop() mantém o servidor ativo.
 * Deve permanecer rápido para evitar bloqueios e watchdog.
 */
void loop()
{
  // Processa as requisicoes do navegador.
  server.handleClient();

  // Mantém a conexão STA sob controle sem bloquear o servidor.
  static unsigned long lastWiFiCheck = 0;
  if (millis() - lastWiFiCheck > 10000)
  {
    lastWiFiCheck = millis();
    if (wifiSSID.length() > 0 && WiFi.status() != WL_CONNECTED)
    {
      WiFi.reconnect();
    }
  }

  // Pequena pausa para evitar loop excessivamente agressivo.
  delay(1);
}
