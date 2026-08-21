# BTP - Binary Telemetry Protocol

BTP é um protocolo binário de telemetria e controle, versionado e testável,
para comunicação entre firmwares embarcados e aplicações de computador sobre
enlaces de baixa banda e alta perda (ESP-NOW) ou de banda maior e orientados a
byte-stream (USB Serial). Este repositório é a fonte canônica da
especificação, do codec compartilhado e dos vetores binários de conformidade.

## Princípios de design

- **Binário e de largura fixa.** Todo campo multi-byte é serializado
  explicitamente em little-endian. Nenhuma implementação transmite a
  representação de memória de uma `struct`: padding, alinhamento, ABI e
  endianness variam entre plataformas e nunca fazem parte do contrato.
- **Payload opaco.** O payload é uma sequência de bytes com tamanho explícito.
  Não é uma string terminada em `0x00`, nem delimitada por CR/LF; qualquer
  byte, incluindo `0x00`, `0x0A` e `0x0D`, é um dado válido.
- **Canais logicamente separados.** `TELEMETRY`, `LOG`, `COMMAND`, `CONTROL`
  e `TERMINAL` nunca compartilham semântica, mesmo quando trafegam no mesmo
  enlace físico.
- **Identidade e tempo na origem.** Um envelope carrega `source_id`,
  `boot_id`, `sequence` e um `timestamp_us` monotônico criados por quem gerou
  o dado. Um gateway roteia e retransmite, mas não os substitui.
- **Detecção de corrupção, não autenticação.** Cada frame carrega um CRC-32
  próprio. Ele detecta corrupção acidental; não autentica origem nem protege
  contra alteração intencional.
- **Sem modo legado.** Não existe fallback, autodetecção ou parser
  alternativo para um formato anterior. Um peer incompatível é rejeitado de
  forma explícita.

## Formato do frame (v1)

Um frame BTP v1 é a concatenação exata de um cabeçalho fixo, o payload e um
CRC-32, sem alinhamento, terminador ou padding entre os campos:

```text
+----------------------+--------------------------+-------------+
| header (36 octetos)  | payload (payload_size)   | CRC32 (4)   |
+----------------------+--------------------------+-------------+
offset 0               offset 36                  offset 36 + N

frame_size = 36 + payload_size + 4
```

O cabeçalho tem exatamente 36 octetos:

| Offset | Tamanho | Campo | Tipo | Significado |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | `magic` | 4 octetos | `42 54 50 00` (`BTP\0`) |
| 4 | 1 | `version` | `uint8` | `0x01` ou `0x02`; `0x02` é selecionado automaticamente quando `ENCRYPTED` está marcado |
| 5 | 1 | `type` | `uint8` | Canal lógico da mensagem |
| 6 | 2 | `flags` | `uint16_le` | Bit `0x0001` = fragmentado; bit `0x0002` = `ENCRYPTED`; bits 2-3 (`0x000C`) = `CIPHER_ID` (`0` AES-128-GCM, `1` ChaCha20-Poly1305); demais reservados — detalhe normativo completo em [`docs/BTP_V1.md`](docs/BTP_V1.md) seção 8 |
| 8 | 2 | `header_size` | `uint16_le` | `36`, detecta mudança futura de layout |
| 10 | 2 | `payload_size` | `uint16_le` | Bytes deste fragmento |
| 12 | 4 | `source_id` | `uint32_le` | Identidade estável e não nula da origem |
| 16 | 4 | `boot_id` | `uint32_le` | Identidade da inicialização, não reusada |
| 20 | 4 | `sequence` | `uint32_le` | Identifica a mensagem lógica dentro do boot |
| 24 | 8 | `timestamp_us` | `uint64_le` | Instante de origem, monotônico, em µs |
| 32 | 2 | `object_id` | `uint16_le` | Objeto no namespace definido por `type` |
| 34 | 1 | `fragment_index` | `uint8` | Índice do fragmento, começando em zero |
| 35 | 1 | `fragment_count` | `uint8` | Total de fragmentos da mensagem lógica |

A identidade canônica de uma mensagem lógica é a tripla
(`source_id`, `boot_id`, `sequence`); todos os fragmentos de uma mensagem
compartilham essa identidade e o mesmo `type`, `flags`, `timestamp_us` e
`object_id`. O CRC-32/ISO-HDLC cobre do primeiro octeto de `magic` até o
último octeto do payload e é escrito imediatamente depois, em little-endian.

Especificação completa, tabela de tipos, regras normativas de validação e
exemplos hexadecimais em [`docs/BTP_V1.md`](docs/BTP_V1.md).

## Canais lógicos

| `type` | Canal | Direção típica | Semântica |
| --- | --- | --- | --- |
| `0x01` | `TELEMETRY` | origem -> host | Amostras best effort identificadas por `source + topic + schema`; sem ACK por amostra. |
| `0x02` | `LOG` | origem/gateway -> host | Eventos e diagnóstico; não substitui telemetria estruturada. |
| `0x03` | `COMMAND` | host <-> origem | Requisição com `request_id`, deduplicação e resultado correlacionado. |
| `0x04` | `TERMINAL` | bidirecional | Entrada/saída de terminal como bytes opacos, isolada dos demais canais. |
| `0x05` | `CONTROL` | bidirecional | Sessão, `HELLO`, manifesto/descoberta, assinaturas e status. |

## Telemetria

Uma amostra `TELEMETRY` usa `object_id` do envelope como `topic_id`. Um
tópico é identificado pelo par `(source_id, topic_id)`, local ao namespace de
cada fonte. O payload lógico começa com um `schema_version` (`uint16_le`)
seguido do corpo codificado; a tripla `(source_id, topic_id, schema_version)`
seleciona o schema — nomes de campo, tipos e unidades são anunciados fora da
amostra, nunca repetidos em cada mensagem. `schema_version` é monotônico por
tópico: mudar o encoding ou o significado de um campo exige uma versão nova,
nunca reinterpretar uma já emitida.

Encodings de corpo suportados: `OPAQUE_BYTES`, `UTF8`, `JSON_UTF8`,
`CSV_UTF8`, `PACKED_LE` e `TLV_LE`. `PACKED_LE` é o encoding padrão de
produção; `CSV_UTF8` fica restrito a teste e diagnóstico. Detalhes de cada
encoding, arrays e regras de schema em [`docs/TELEMETRY.md`](docs/TELEMETRY.md).

## Comandos, manifesto e sessão

Toda requisição de `COMMAND` carrega a identidade do emissor
(`request_source_id`, `request_boot_id`) e o `sequence` do envelope funciona
como `request_id`; uma resposta referencia essa tripla em
`reply_to_sequence` para correlação e deduplicação. Um conjunto comum de
códigos de resultado (`SUCCESS`, `REJECTED`, `FAILED`, `TIMEOUT`,
`CANCELLED`, `UNSUPPORTED`, `BUSY`) e de erro cobre todos os objetos de
`COMMAND` e `CONTROL`.

O canal `CONTROL` define a negociação `HELLO`, o manifesto de fontes/tópicos/
ações para descoberta, o modelo de assinatura e controle de taxa por tópico,
o status periódico e o protocolo de sessão da serial. Layout completo de cada
objeto em [`docs/COMMANDS_AND_ACTIONS.md`](docs/COMMANDS_AND_ACTIONS.md).

## Transportes

O envelope e o CRC não mudam entre transportes; apenas os limites de tamanho
e o framing do enlace são diferentes:

| | ESP-NOW | Serial (modo protocolado) | USB HID |
| --- | ---: | ---: | ---: |
| Frame BTP máximo | 250 octetos | 4096 octetos | 62 octetos |
| Payload máximo | 210 octetos | 4056 octetos | 22 octetos |
| Framing do enlace | 1 datagrama = 1 frame completo | `0x00 \| COBS(frame) \| 0x00` | relatório HID (report ID + prefixo de tamanho + frame) |

Em ESP-NOW, cada datagrama contém exatamente um frame BTP, sem prefixo,
delimitador ou padding — tamanho exato `40 + payload_size`. Na serial, o modo
protocolado aplica COBS ao frame inteiro (o bloco codificado nunca contém
`0x00`) e alterna com um modo de console humano via um handshake textual
(`BTP/1 ENTER <nonce>` / `BTP/1 READY <nonce>`); a posse exclusiva da porta e
o encerramento de sessão são definidos no documento de transporte. Em USB
HID, um relatório de 64 octetos (1 de Report ID, 1 de prefixo de tamanho —
necessário porque um relatório de tamanho fixo sempre transmite os 63
octetos de dado completos, preenchidos com zero quando a escrita é menor —,
62 de frame BTP) já é a unidade de framing entregue pelo host — sem COBS e
sem modo console, a interface fica sempre em modo protocolado.

Mensagens lógicas maiores que o payload do transporte usam fragmentação:
cada fragmento é um frame BTP completo e independente (CRC próprio), até 255
fragmentos por mensagem, remontados na origem por `fragment_index` e
`fragment_count`. Em USB HID isso vale até para mensagens de controle
pequenas como `HELLO`, dado o teto de 22 octetos de payload por relatório.

Detalhes normativos em [`docs/TRANSPORT_ESPNOW.md`](docs/TRANSPORT_ESPNOW.md),
[`docs/TRANSPORT_SERIAL.md`](docs/TRANSPORT_SERIAL.md) e
[`docs/TRANSPORT_USB_HID.md`](docs/TRANSPORT_USB_HID.md); COBS, decoder
incremental, fragmentação e reassembly compartilhados em
[`docs/STREAM_AND_REASSEMBLY.md`](docs/STREAM_AND_REASSEMBLY.md).

## Criptografia (v2.0)

O bit `ENCRYPTED` (`0x0002`) em `flags` sinaliza que o payload lógico de uma
mensagem deixou de ser dado em claro e passa a ser `ciphertext ‖ tag`; um
frame que o marca também passa a usar `version == 0x02` no envelope,
selecionado automaticamente pelo encoder. Com `ENCRYPTED` limpo, nada disso
se aplica e o frame segue integralmente o formato descrito acima.

Duas cifras AEAD são suportadas, identificadas no wire pelo sub-campo
`CIPHER_ID` (bits 2-3 de `flags`, máscara `0x000C`): `0` para AES-128-GCM,
primária e acelerada por hardware nos ESP32-S3 do projeto, e `1` para
ChaCha20-Poly1305, alternativa constante em tempo para chips sem acelerador
AES. As duas cifras usam tamanhos de chave diferentes e não intercambiáveis
— 16 octetos para AES-128-GCM, 32 para ChaCha20-Poly1305 (RFC 8439) — ver
[`docs/BTP_V1.md`](docs/BTP_V1.md) seções 8.1 e 8.5.

O nonce de 96 bits exigido pelas duas cifras é montado só com campos que o
cabeçalho já carrega — `source_id ‖ boot_id ‖ sequence` —, sem octeto novo
no wire; como esses três campos já são obrigatoriamente únicos por regras
normativas independentes desta extensão (`docs/BTP_V1.md` seção 6), a
combinação nunca se repete sob a mesma chave (argumento completo em
`docs/BTP_V1.md` seção 8.2). Os dados
associados (AAD) são os 36 octetos do cabeçalho, completos: continuam em
claro, para que roteamento e validação operem antes de decifrar, mas
qualquer alteração neles invalida o tag do mesmo jeito que alterar o
payload invalidaria. O tag de 16 octetos é calculado e verificado no nível
da mensagem lógica — antes de fragmentar, na origem; depois de reassemblar,
no receptor — nunca por fragmento individual, já que `sequence` identifica
a mensagem lógica, não o frame físico.

A chave, no tamanho correspondente à cifra em uso, nunca trafega em nenhum
campo do wire; como a própria decisão de habilitar `ENCRYPTED` em um canal,
ela é provisionada fora de banda entre os dois endpoints. O perfil `UsbHid`
fica deliberadamente fora do escopo desta primeira versão: um tag de 16
octetos sobre um payload de só 22 octetos dominaria o orçamento do
transporte, e a política adequada para esse caso (provavelmente
truncamento) fica para revisão futura — motivação completa na ADR abaixo.

A cifragem em si vive em [`include/btp/aead.hpp`](include/btp/aead.hpp) e
[`src/aead.cpp`](src/aead.cpp), módulo `btp::aead` separado do codec: o
codec continua só fazendo framing, tratando o payload cifrado como
`ciphertext ‖ tag` opaco sem chamar nenhuma cifra, e permanece sem
dependências. É o módulo `btp::aead`, sozinho, que depende de mbedtls para
implementar as duas cifras (veja "Build e testes" abaixo).

Especificação normativa completa em [`docs/BTP_V1.md`](docs/BTP_V1.md)
seção 8 e decisão de design em
[`docs/decisions/0012-criptografia-aead-payload.md`](docs/decisions/0012-criptografia-aead-payload.md).

## Versionamento

`version` no envelope identifica o wire format e permite rejeição explícita
de incompatibilidade. O repositório publica `vMAJOR.MINOR.PATCH` cobrindo em
conjunto a especificação, a biblioteca e os vetores de conformidade daquela
revisão, seguindo SemVer: mudança incompatível de bytes/semântica é `MAJOR`,
extensão compatível e negociável é `MINOR`, correção sem efeito observável no
wire é `PATCH`. Política completa em [`docs/VERSIONING.md`](docs/VERSIONING.md).

## Organização do repositório

```text
BTP/
|-- README.md
|-- CONTRIBUTING.md
|-- CMakeLists.txt
|-- library.json
|-- include/btp/
|   |-- aead.hpp
|   |-- codec.hpp
|   |-- fragmentation.hpp
|   `-- stream.hpp
|-- src/
|   |-- aead.cpp
|   |-- codec.cpp
|   |-- fragmentation.cpp
|   `-- stream.cpp
|-- tests/
|-- test-vectors/v1/
|   |-- valid/
|   |-- invalid/
|   `-- manifest.json
|-- test-vectors/v2/
|   |-- valid/
|   |-- invalid/
|   `-- manifest.json
|-- tools/test_vectors.py
|-- tools/test_vectors_v2.py
`-- docs/
    |-- ARCHITECTURE.md
    |-- BTP_V1.md
    |-- CODEC.md
    |-- CONFORMANCE.md
    |-- STREAM_AND_REASSEMBLY.md
    |-- COMMANDS_AND_ACTIONS.md
    |-- TELEMETRY.md
    |-- TRANSPORT_ESPNOW.md
    |-- TRANSPORT_SERIAL.md
    |-- VERSIONING.md
    `-- decisions/
```

As decisões de arquitetura aceitas ficam em
[`docs/decisions/`](docs/decisions/README.md) como ADRs imutáveis. Os fluxos
entre os projetos que consomem o protocolo (produção de telemetria, gateway,
apresentação) estão em [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## Codec e biblioteca compartilhada

O codec canônico ([`include/btp/codec.hpp`](include/btp/codec.hpp),
[`src/codec.cpp`](src/codec.cpp)) requer apenas C++11, não aloca memória e não
depende de Arduino, ESP-IDF, Qt ou de um sistema operacional específico.
Payloads são representados por uma view (ponteiro + tamanho); o encoder
escreve em um buffer fornecido pelo chamador e o decoder valida magic,
versão, tamanho, limite do transporte, CRC e invariantes de fragmentação,
nessa ordem, antes de publicar qualquer campo.

COBS, o decoder serial incremental, o fragmentador e o reassembly ficam em
[`include/btp/stream.hpp`](include/btp/stream.hpp) e
[`include/btp/fragmentation.hpp`](include/btp/fragmentation.hpp), com a
mesma ausência de alocação dinâmica. Detalhes de API em
[`docs/CODEC.md`](docs/CODEC.md).

### Build e testes

```text
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
pio run -d tests/embedded
```

`ctest` cobre o codec, o transporte (COBS/fragmentação/reassembly) e os
vetores canônicos de conformidade — geração byte a byte a partir das
descrições JSON e decodificação independente de cada `.bin`, incluindo o
motivo exato de cada caso inválido. Vetores em
[`test-vectors/v1/`](test-vectors/v1/README.md), documentados em
[`docs/CONFORMANCE.md`](docs/CONFORMANCE.md). Com a opção CMake
`BTP_ENABLE_AEAD` (ligada por padrão), `cmake`/`ctest` também buscam o
mbedtls via `FetchContent` e exercitam os testes unitários de `btp::aead`
e a prova real de decodificação cruzada dos vetores de `test-vectors/v2/`;
desligada, a dependência e o alvo `btp::aead` são pulados por completo.

### Consumo

A biblioteca é consumida pelo alvo CMake `btp::codec` ou pelo manifesto
PlatformIO [`library.json`](library.json). Um consumidor deve fixar uma
versão publicada (tag ou revisão imutável) e executar os mesmos vetores de
conformidade diretamente desta dependência; não é permitido manter cópias
independentes da especificação, do codec ou dos vetores.

## Como contribuir

Leia [`CONTRIBUTING.md`](CONTRIBUTING.md). Qualquer mudança de wire format
deve vir acompanhada de decisão documentada (ADR), classificação SemVer e
vetores de teste antes de ser aceita.
