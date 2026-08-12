# BTP - Binary Telemetry Protocol

O **BTP (Binary Telemetry Protocol)** é um protocolo binário de comunicação e
plotagem de dados em tempo real entre firmwares ESP32 (via ESP-NOW) e
aplicações de computador. Este repositório é a fonte canônica da
especificação, das decisões de arquitetura, do codec e dos vetores de
conformidade — independente de qualquer robô específico, para poder ser
reaproveitado em outros projetos.

## Estado atual

O BTP v1 está em fase de especificação. A fundação, as responsabilidades, o
[wire format do envelope](docs/BTP_V1.md), os [payloads de
telemetria](docs/TELEMETRY.md), os [comandos, manifesto, sessão e
terminal](docs/COMMANDS_AND_ACTIONS.md) e os transportes
[ESP-NOW](docs/TRANSPORT_ESPNOW.md) e [Serial/COBS](docs/TRANSPORT_SERIAL.md)
estão documentados. O [codec BTP v1](docs/CODEC.md) portátil e sem alocação
dinâmica já codifica e valida o envelope. Os utilitários compartilhados de
[COBS, decoder incremental, fragmentação e reassembly](docs/STREAM_AND_REASSEMBLY.md)
também estão implementados. Os [vetores canônicos de
conformidade](docs/CONFORMANCE.md) completam a referência binária comum para as
toolchains ESP-IDF, Arduino e Qt/desktop.

Não existe nem será criado suporte ao protocolo legado. Até a publicação de
uma versão identificada, nenhum consumidor deve tratar o conteúdo atual como
um contrato estável.

## Projetos consumidores

| Projeto | Responsabilidade no BTP |
| --- | --- |
| `Bally_OS` | Produzir telemetria e logs, executar comandos e originar timestamps no robô. |
| `Bally_dongle` | Atuar como gateway entre ESP-NOW e USB Serial, rotear canais, manter catálogo e ações persistidas. |
| `TraceView` | Descobrir fontes e tópicos, apresentar telemetria e enviar intenções de comando; não definir a semântica dos comandos. |

Consulte [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) para os fluxos e os
limites de responsabilidade.

## Princípios do contrato

- O protocolo é binário, versionado e testável.
- Payloads são sequências opacas de bytes com tamanho explícito; não são
  strings e não usam `0x00`, CR ou LF como terminador.
- Valores no wire usam larguras fixas e serialização little-endian.
- Estruturas C/C++ nunca são transmitidas por `reinterpret_cast` ou cópia de
  memória bruta.
- `LOG`, `TELEMETRY`, `COMMAND` e `TERMINAL` são canais lógicos distintos.
- Telemetria identifica `source + topic + field`; um tópico nunca representa
  um gráfico específico.
- O timestamp nasce na origem e não é substituído pelo dongle.
- CRC detecta corrupção, mas não autentica origem nem conteúdo.

## Pendências

- Transporte USB nativo full-speed (sem depender de porta serial) — ver
  [`topicos/23_transporte_usb_nativo.txt`](topicos/23_transporte_usb_nativo.txt).

## Organização

```text
BTP/
|-- README.md
|-- CONTRIBUTING.md
|-- CMakeLists.txt
|-- library.json
|-- PLANO_GERAL.txt
|-- include/btp/
|   |-- codec.hpp
|   |-- fragmentation.hpp
|   `-- stream.hpp
|-- src/
|   |-- codec.cpp
|   |-- fragmentation.cpp
|   `-- stream.cpp
|-- tests/
|-- test-vectors/v1/
|   |-- valid/
|   |-- invalid/
|   `-- manifest.json
|-- tools/test_vectors.py
|-- docs/
|   |-- ARCHITECTURE.md
|   |-- BTP_V1.md
|   |-- CODEC.md
|   |-- CONFORMANCE.md
|   |-- STREAM_AND_REASSEMBLY.md
|   |-- COMMANDS_AND_ACTIONS.md
|   |-- TELEMETRY.md
|   |-- TRANSPORT_ESPNOW.md
|   |-- TRANSPORT_SERIAL.md
|   |-- VERSIONING.md
|   `-- decisions/
`-- topicos/
```

As decisões aceitas ficam em [`docs/decisions/`](docs/decisions/README.md).
A política de releases e compatibilidade está em
[`docs/VERSIONING.md`](docs/VERSIONING.md).

## Consumo e distribuição

Especificação e código compartilhado pertencem a este repositório. Os projetos
consumidores devem fixar uma versão publicada (tag, pacote ou revisão imutável)
e integrar o artefato a partir daqui. Não é permitido manter cópias
independentes da especificação, do codec ou de arquivos-fonte compartilhados
nos repositórios consumidores.

A biblioteca pode ser consumida pelo alvo CMake `btp::codec` ou pelo manifesto
PlatformIO [`library.json`](library.json), conforme [documentação do
codec](docs/CODEC.md). Os consumidores devem executar os mesmos
[vetores de conformidade](docs/CONFORMANCE.md) diretamente desta dependência.
Publicar pacotes e releases continua exigindo ação
explícita do mantenedor; esses mecanismos não alteram a regra de fonte
canônica única.

## Como contribuir

Leia [`CONTRIBUTING.md`](CONTRIBUTING.md). Qualquer mudança de wire format deve
ser acompanhada de decisão documentada, classificação de versão e vetores de
teste antes de ser aceita.
