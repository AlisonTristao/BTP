# BTP — Binary Telemetry Protocol

Protocolo binário de telemetria e controle entre firmware embarcado e
aplicações de computador, sobre enlaces de baixa banda e alta perda (ESP-NOW)
ou de banda maior e orientados a byte-stream (USB Serial/HID). Este
repositório é a fonte canônica de três artefatos que versionam juntos: a
**especificação** do wire, a **biblioteca** C++11 que a implementa e os
**vetores binários** de conformidade.

📖 **Documentação completa:** <https://alisontristao.github.io/BTP/> — os
mesmos arquivos de [`docs/`](docs/index.md), organizados como leitura
sequencial.

| | |
| --- | --- |
| Wire format | v1 (`version == 0x01`) e v2 (`version == 0x02`, payload AEAD) |
| Biblioteca | C++11, sem alocação dinâmica, sem dependência de Arduino/ESP-IDF/Qt/SO |
| Alvos | `btp::codec` (sem dependências) e `btp::aead` (mbedtls, opcional) |
| Versão | `2.0.0-beta` no fonte; última tag publicada `v1.1.0-beta`, linha v1 na branch `1.x` |

## Invariantes de projeto

- **Serialização explícita.** Todo campo multi-byte é escrito em
  little-endian, campo por campo. Nenhuma implementação transmite a
  representação de memória de uma `struct`, nem deriva tamanho de `sizeof`,
  alinhamento ou ABI.
- **Payload opaco.** Sequência de octetos com tamanho explícito — não é
  string terminada em `0x00` nem delimitada por CR/LF. `0x00`, `0x0A` e
  `0x0D` são dados válidos.
- **Canais logicamente separados.** `TELEMETRY`, `LOG`, `COMMAND`, `CONTROL`
  e `TERMINAL` nunca compartilham semântica, mesmo no mesmo enlace físico.
- **Identidade e tempo na origem.** `source_id`, `boot_id`, `sequence` e
  `timestamp_us` são criados por quem gerou o dado; um gateway roteia e
  retransmite, mas não os substitui.
- **CRC detecta corrupção, não autentica.** Autenticidade e confidencialidade
  são o papel da extensão AEAD da v2, não do CRC.
- **Sem modo legado.** Não existe fallback, autodetecção ou parser
  alternativo. Peer incompatível é rejeitado explicitamente.

## Frame

```text
+----------------------+--------------------------+-------------+
| header (36 octetos)  | payload (payload_size)   | CRC32 (4)   |
+----------------------+--------------------------+-------------+
offset 0               offset 36                  offset 36 + N

frame_size = 40 + payload_size
```

| Offset | Tam. | Campo | Tipo | Significado |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | `magic` | 4 octetos | `42 54 50 00` (`BTP\0`) |
| 4 | 1 | `version` | `uint8` | `0x01`, ou `0x02` quando `ENCRYPTED` está marcado |
| 5 | 1 | `type` | `uint8` | Canal lógico da mensagem |
| 6 | 2 | `flags` | `uint16_le` | `0x0001` `FRAGMENTED`; `0x0002` `ENCRYPTED`; bits 2-3 (`0x000C`) `CIPHER_ID`; demais reservados |
| 8 | 2 | `header_size` | `uint16_le` | `36`, detecta mudança futura de layout |
| 10 | 2 | `payload_size` | `uint16_le` | Octetos deste fragmento |
| 12 | 4 | `source_id` | `uint32_le` | Identidade estável e não nula da origem |
| 16 | 4 | `boot_id` | `uint32_le` | Identidade da inicialização, não reusada |
| 20 | 4 | `sequence` | `uint32_le` | Identifica a mensagem lógica dentro do boot |
| 24 | 8 | `timestamp_us` | `uint64_le` | Instante de origem, monotônico, em µs |
| 32 | 2 | `object_id` | `uint16_le` | Objeto no namespace definido por `type` |
| 34 | 1 | `fragment_index` | `uint8` | Índice do fragmento, começando em zero |
| 35 | 1 | `fragment_count` | `uint8` | Total de fragmentos da mensagem lógica |

A identidade canônica de uma mensagem lógica é a tripla (`source_id`,
`boot_id`, `sequence`); todos os fragmentos dela compartilham essa tripla e o
mesmo `type`, `flags`, `timestamp_us` e `object_id`. O CRC-32/ISO-HDLC cobre
de `magic` ao último octeto do payload e é gravado em seguida, em
little-endian. Regras normativas completas, tabela de tipos e exemplos
hexadecimais em [`docs/BTP_V1.md`](docs/BTP_V1.md).

## Canais

| `type` | Canal | Direção | Semântica |
| --- | --- | --- | --- |
| `0x01` | `TELEMETRY` | origem → host | Amostras best effort por `source + topic + schema`; sem ACK por amostra. |
| `0x02` | `LOG` | origem/gateway → host | Eventos e diagnóstico; não substitui telemetria estruturada. |
| `0x03` | `COMMAND` | host ↔ origem | Requisição com `request_id`, deduplicação e resultado correlacionado. |
| `0x04` | `TERMINAL` | bidirecional | Entrada/saída de terminal como bytes opacos, isolada dos demais canais. |
| `0x05` | `CONTROL` | bidirecional | Sessão, `HELLO`, manifesto/descoberta, assinaturas e status. |

Payload lógico de cada canal: [`docs/TELEMETRY.md`](docs/TELEMETRY.md)
(identidade de tópico, schemas, encodings, arrays) e
[`docs/COMMANDS_AND_ACTIONS.md`](docs/COMMANDS_AND_ACTIONS.md) (comandos,
manifesto, assinatura, status, sessão, terminal).

## Transportes

O envelope e o CRC não mudam entre transportes; mudam os limites e o framing
do enlace.

| | ESP-NOW | Serial (protocolado) | USB HID |
| --- | ---: | ---: | ---: |
| Frame máximo | 250 octetos | 4096 octetos | 62 octetos |
| Payload máximo | 210 octetos | 4056 octetos | 22 octetos |
| Framing | 1 datagrama = 1 frame (`40 + payload_size`) | `0x00 ‖ COBS(frame) ‖ 0x00` | relatório de 64: Report ID + prefixo de tamanho + 62 de frame |
| Console humano | — | sim, alternado por handshake textual | não; sempre protocolado |

Mensagem lógica maior que o payload do transporte é fragmentada: cada
fragmento é um frame completo e independente, com CRC próprio, até 255
fragmentos, remontados por `fragment_index`/`fragment_count`. No USB HID isso
vale até para mensagens pequenas de controle, dado o teto de 22 octetos.

Normativo em [`docs/TRANSPORT_ESPNOW.md`](docs/TRANSPORT_ESPNOW.md),
[`docs/TRANSPORT_SERIAL.md`](docs/TRANSPORT_SERIAL.md) e
[`docs/TRANSPORT_USB_HID.md`](docs/TRANSPORT_USB_HID.md); COBS, decoder
incremental, fragmentação e reassembly compartilhados em
[`docs/STREAM_AND_REASSEMBLY.md`](docs/STREAM_AND_REASSEMBLY.md).

## Criptografia (v2)

Com o bit `ENCRYPTED` (`0x0002`) marcado, o payload lógico passa a ser
`ciphertext ‖ tag` e o envelope passa a `version == 0x02`. Nada mais muda: o
header segue em claro, o CRC segue por frame e a fragmentação opera igual.

| | |
| --- | --- |
| Cifras | `CIPHER_ID` = `0` AES-128-GCM (padrão, acelerada em hardware); `1` ChaCha20-Poly1305. `2`/`3` reservados e rejeitados. |
| Chave | 16 octetos (AES-128-GCM) ou 32 (ChaCha20-Poly1305, RFC 8439) — não intercambiáveis. Nunca trafega no wire. |
| Nonce | `source_id ‖ boot_id ‖ sequence`, os 96 bits exigidos, sem octeto novo no header. |
| AAD | Os 36 octetos do header — em claro no wire, mas autenticados. |
| Tag | 16 octetos, no nível da mensagem lógica: cifra antes de fragmentar, verifica depois de remontar. |
| Fora de escopo | Perfil `UsbHid` (tag de 16 sobre payload de 22); anti-replay; rotação de chave. |

Implementação em [`include/btp/aead.hpp`](include/btp/aead.hpp) /
[`src/aead.cpp`](src/aead.cpp), no alvo `btp::aead` — o único que depende de
mbedtls. `btp::codec` continua sem dependência nenhuma e trata payload
cifrado como bytes opacos.

Capítulo de leitura em [`docs/CRYPTO.md`](docs/CRYPTO.md), norma na seção 8 de
[`docs/BTP_V1.md`](docs/BTP_V1.md), decisão na
[ADR 0012](docs/decisions/0012-criptografia-aead-payload.md) — ainda em
`Proposta`, porque nenhum dos três consumidores implementou a cifra.

## Biblioteca

| Alvo | Fontes | Dependências |
| --- | --- | --- |
| `btp::codec` | `src/codec.cpp`, `src/fragmentation.cpp`, `src/stream.cpp` | nenhuma além da biblioteca padrão C++11 |
| `btp::aead` | `src/aead.cpp` | `btp::codec` + `mbedcrypto` (privado) |

O codec não aloca memória: payloads são views (ponteiro + tamanho), o encoder
escreve em buffer do chamador e o decoder devolve uma view para dentro do
frame de entrada. `decode()` valida magic, versão, tamanho, limite do
transporte, CRC e invariantes de fragmentação — nessa ordem — antes de
publicar qualquer campo. API em [`docs/CODEC.md`](docs/CODEC.md).

### Build e testes

```text
cmake -S . -B build -G Ninja        # BTP_ENABLE_AEAD=ON, BTP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
pio run -d tests/embedded           # build de sanidade no alvo embarcado
```

| Teste (`ctest`) | Cobre |
| --- | --- |
| `btp_codec_tests` | Encode/decode, validação de header, flags e `CIPHER_ID` |
| `btp_transport_tests` | COBS, decoder incremental, fragmentação e reassembly |
| `btp_conformance_tests` | Vetores v1: geração byte a byte e decode independente |
| `btp_conformance_v2_tests` | Vetores v2: framing da extensão AEAD, incluindo os casos inválidos |
| `btp_vector_descriptions[_v2]` | Consistência `.json` ↔ `.bin` via `tools/test_vectors*.py` |
| `btp_aead_tests` | Round-trip das duas cifras, tag corrompido, AAD alterado, chave inválida |
| `btp_aead_conformance_tests` | Decifra os vetores v2 de verdade e compara com o plaintext documentado |

`-DBTP_ENABLE_AEAD=OFF` remove a dependência de mbedtls, o alvo `btp::aead` e
seus dois testes; o resto compila igual. Com a opção ligada, o mbedtls é
obtido por `FetchContent` fixado em tag, sem exigir instalação no sistema.

### Consumo

Pelo alvo CMake `btp::codec` (e `btp::aead`, se for cifrar) ou pelo manifesto
PlatformIO [`library.json`](library.json). Um consumidor **deve** fixar uma
versão publicada (tag ou revisão imutável) e executar os vetores de
conformidade a partir desta dependência. Manter cópia própria da
especificação, do codec ou dos vetores não é permitido —
[ADR 0001](docs/decisions/0001-fonte-canonica-e-sem-legado.md).

## Vetores de conformidade

[`test-vectors/v1/`](test-vectors/v1/README.md) e
[`test-vectors/v2/`](test-vectors/v2/README.md) são a referência binária
canônica. Cada vetor é um par de mesmo nome: `.json` com a descrição legível
(campos por valor, payload em hexadecimal) e `.bin` com o frame cru, de
`magic` a CRC, sem COBS ou wrapper de transporte. `manifest.json` enumera os
vetores e a ordem de chegada do cenário com duas fontes fragmentadas.
Contrato dos arquivos e como executá-los em
[`docs/CONFORMANCE.md`](docs/CONFORMANCE.md).

```text
python tools/test_vectors.py    --root test-vectors/v1 --check
python tools/test_vectors_v2.py --root test-vectors/v2 --check
```

## Versionamento

Uma linha `vMAJOR.MINOR.PATCH` versiona em conjunto a especificação, a
biblioteca e os vetores daquela revisão. Mudança incompatível de bytes ou de
semântica é `MAJOR`; extensão compatível e negociável é `MINOR`; correção sem
efeito observável no wire é `PATCH`. A extensão AEAD é classificada `MAJOR`
(`v2.0.0`) porque um decoder v1.x **MUST** rejeitar frame com bit reservado
marcado. Política completa em [`docs/VERSIONING.md`](docs/VERSIONING.md).

## Organização do repositório

```text
BTP/
|-- CMakeLists.txt              alvos btp::codec e btp::aead, testes, FetchContent do mbedtls
|-- library.json                manifesto PlatformIO
|-- mkdocs.yml                  sumário e tema do livro em docs/
|-- mkdocs_hooks.py             reescreve, no build do site, links que saem de docs/
|-- requirements-docs.txt       dependências só da documentação
|-- include/btp/                codec.hpp, aead.hpp, fragmentation.hpp, stream.hpp
|-- src/                        codec.cpp, aead.cpp, fragmentation.cpp, stream.cpp
|-- tests/                      testes nativos + tests/embedded (PlatformIO)
|-- test-vectors/v1/            valid/, invalid/, manifest.json
|-- test-vectors/v2/            idem, para a extensão AEAD
|-- tools/                      test_vectors.py, test_vectors_v2.py
|-- .github/workflows/docs.yml  publica docs/ no GitHub Pages
`-- docs/                       o livro (ver abaixo)
```

## Documentação

| Parte | Capítulos |
| --- | --- |
| Panorama | [ARCHITECTURE](docs/ARCHITECTURE.md), [WALKTHROUGH](docs/WALKTHROUGH.md), [CONVENTIONS](docs/CONVENTIONS.md) |
| Contrato no wire | [BTP_V1](docs/BTP_V1.md), [CODEC](docs/CODEC.md), [STREAM_AND_REASSEMBLY](docs/STREAM_AND_REASSEMBLY.md) |
| Canais lógicos | [TELEMETRY](docs/TELEMETRY.md), [COMMANDS_AND_ACTIONS](docs/COMMANDS_AND_ACTIONS.md) |
| Transportes | [TRANSPORT_SERIAL](docs/TRANSPORT_SERIAL.md), [TRANSPORT_ESPNOW](docs/TRANSPORT_ESPNOW.md), [TRANSPORT_USB_HID](docs/TRANSPORT_USB_HID.md) |
| Criptografia | [CRYPTO](docs/CRYPTO.md) |
| Processo | [CONFORMANCE](docs/CONFORMANCE.md), [VERSIONING](docs/VERSIONING.md), [CONTRIBUTING](docs/CONTRIBUTING.md) |
| Apêndices | [decisions/](docs/decisions/README.md) (ADR 0001–0012), [integration-captures/](docs/integration-captures/README.md) |

Para servir o livro localmente:

```text
python -m venv .venv-docs
.venv-docs/Scripts/pip install -r requirements-docs.txt   # Linux/macOS: .venv-docs/bin/pip
mkdocs serve
```

## Como contribuir

Leia [`docs/CONTRIBUTING.md`](docs/CONTRIBUTING.md). Toda mudança de wire
format exige, no mesmo conjunto de alterações: ADR, atualização da
especificação canônica, classificação SemVer, vetores válidos e inválidos, e
implementação equivalente nas plataformas afetadas. Uma mudança não está
completa se apenas um consumidor consegue codificá-la.
