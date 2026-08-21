# BTP: protocolo binário de telemetria e controle

O BTP transporta telemetria, logs, comandos e terminal entre um firmware
embarcado e uma aplicação de computador, atravessando enlaces com
características opostas: ESP-NOW, de banda baixa e perda alta, e USB
(Serial/HID), de banda maior e orientado a byte-stream. Cada mensagem carrega
a identidade e o instante criados na origem, e nenhum intermediário os
reescreve.

Este repositório é a fonte canônica de três coisas ao mesmo tempo: a
especificação do wire format, a biblioteca C++ que a implementa e os vetores
binários que provam que uma implementação está correta. Este livro é a leitura
sequencial desse conjunto — do panorama de arquitetura até o octeto no
enlace.

## Estado atual

| Componente | Estado |
| --- | --- |
| Wire format v1 (`version == 0x01`) | Especificado, implementado e coberto por vetores. Última release publicada: `v1.1.0-beta`. |
| Wire format v2 (`version == 0x02`, payload AEAD) | Especificado em [BTP_V1.md §8](BTP_V1.md#8-criptografia-aead-do-payload) e implementado na biblioteca (`btp::aead`, duas cifras, vetores em `test-vectors/v2/`). Sem release publicada. |
| [ADR 0012](decisions/0012-criptografia-aead-payload.md) (criptografia) | `Proposta` — vira `Aceita` quando os três consumidores implementarem a cifra de verdade, não só a biblioteca. |
| Linha 1.x | Mantida na branch `1.x`, cortada antes de a v2 avançar em `main`. |

## Como ler

Três caminhos, dependendo do que você veio fazer:

**Vou consumir o BTP em um projeto** (firmware, dongle, desktop) — comece por
[Arquitetura e domínios](ARCHITECTURE.md), siga para
[Do sensor à tela](WALKTHROUGH.md), leia o [Codec portátil](CODEC.md) e o
capítulo do transporte que você vai usar. Só volte à especificação normativa
quando precisar do layout exato de um campo.

**Vou implementar o protocolo em outra plataforma** (outra linguagem, outro
chip) — leia [Convenções e glossário](CONVENTIONS.md), depois
[Frame BTP v1](BTP_V1.md) inteiro, os payloads lógicos de
[Telemetria](TELEMETRY.md) e [Comandos](COMMANDS_AND_ACTIONS.md), o perfil de
transporte aplicável e, obrigatoriamente,
[Vetores de conformidade](CONFORMANCE.md): sua implementação não está pronta
antes de produzir e consumir os mesmos bytes dos vetores.

**Vim entender por que uma decisão é assim** — vá direto ao
[registro de decisões](decisions/README.md). Cada ADR guarda contexto,
alternativas rejeitadas e consequências; os capítulos de especificação
descrevem o que vale hoje, os ADRs descrevem por que passou a valer.

## As seis partes

| Parte | O que cobre |
| --- | --- |
| I — Panorama | Quem produz, roteia e apresenta os dados; o caminho completo de uma amostra; o vocabulário usado no resto do livro. |
| II — O contrato no wire | O frame v1 octeto a octeto, a API do codec e o que COBS, fragmentação e reassembly fazem. |
| III — Canais lógicos | O payload de cada canal: amostras e schemas de telemetria; comandos, manifesto, sessão e terminal. |
| IV — Perfis de transporte | O que muda entre Serial, ESP-NOW e USB HID — limites, framing e posse do enlace. |
| V — Criptografia | O modelo de segurança do payload cifrado, as decisões por trás dele e a API `btp::aead`. |
| VI — Processo e garantias | Os vetores canônicos, a política de versão conjunta e as regras para mudar o wire. |

Os apêndices trazem os ADRs e as capturas de hardware guardadas para
depuração.

## O que este livro não é

Não é tutorial de ESP-IDF, Arduino ou Qt, e não descreve a implementação
interna de nenhum consumidor: `bally_OS` (firmware do robô), `bally_dongle`
(gateway) e `TraceView` (apresentação) têm documentação própria. Aqui está só
o que os três precisam concordar entre si.

Quando um capítulo de leitura e a especificação normativa divergirem, a
especificação vence: [BTP_V1.md](BTP_V1.md) é a fonte canônica do wire, e
[CONVENTIONS.md](CONVENTIONS.md) explica como as palavras **MUST**/**SHOULD**
devem ser lidas nela.
