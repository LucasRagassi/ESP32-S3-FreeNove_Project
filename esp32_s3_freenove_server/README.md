# ESP32-S3-FreeNove — servidor GPIO e ADC

Projeto PlatformIO independente para a placa ESP32-S3 WROOM FreeNove. O ESP32
é o único equipamento e hospeda uma página web protegida para configurar e
acompanhar GPIOs digitais e entradas ADC.

## Recursos

- servidor HTTP com autenticação Basic;
- página principal em LittleFS;
- página de configuração Wi-Fi;
- modo AP permanente para recuperação;
- conexão STA ao roteador por DHCP ou IP fixo;
- configuração individual de GPIO como entrada, saída ou ADC;
- controle HIGH/LOW das saídas;
- estados e modos salvos no NVS.

Este projeto não contém Arduino UNO, W5100, comunicação TCP externa nem página
de informações SPI.

## Estrutura

```text
platformio.ini
src/main.cpp
data/index.html
data/config.html
```

## Gravação

Abra esta pasta no VS Code com PlatformIO e execute:

```bash
pio run -t clean
pio run -t upload
pio run -t uploadfs
pio device monitor
```

O firmware e o LittleFS são gravações separadas. Execute `uploadfs` sempre que
alterar `index.html` ou `config.html`.

## Acesso inicial

- AP: `ESP32-S3-FreeNove`
- Senha do AP: `12345678`
- Endereço do AP: `192.168.4.1`
- Usuário web: `admin`
- Senha web: `esp32admin`

Altere as credenciais no firmware antes de usar o equipamento em uma rede não
confiável. HTTP Basic autentica as rotas, mas não criptografa o tráfego.
