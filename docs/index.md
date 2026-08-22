# BTP: protocolo binário de telemetria e controle

O BTP transporta telemetria, logs, comandos e terminal entre um produtor
embarcado e um consumidor, atravessando enlaces com características opostas:
rádio de banda baixa, datagrama curto e perda alta, e barramento local de banda
maior, orientado a byte-stream. Cada mensagem carrega a identidade e o instante
criados na origem, e nenhum intermediário os reescreve.

Este repositório é a fonte canônica de três coisas ao mesmo tempo: a
especificação do wire format, a biblioteca C++ que a implementa e os vetores
binários que provam que uma implementação está correta. Este livro é a leitura
sequencial desse conjunto — do modelo de papéis até o octeto no enlace.

## Estado atual

| Componente | Estado |
| --- | --- |
| Wire `version == 0x01` | Especificado, implementado e coberto por vetores em `test-vectors/v1/`. |
| Wire `version == 0x02` (payload AEAD) | Especificado em [§8 do frame no wire](BTP_V1.md#8-criptografia-aead-do-payload) e implementado na biblioteca (`btp::aead`, duas cifras), com vetores em `test-vectors/v2/`. |
| Última release publicada | `v1.1.0-beta`. Nenhuma tag do wire `0x02` foi publicada; a versão do artefato vive em [`library.json`](../library.json). |
| Branch `1.x` | Linha de manutenção do wire `0x01`, cortada antes de a `main` avançar para o wire `0x02`. |
| [ADR 0012](decisions/0012-criptografia-aead-payload.md) (criptografia) | `Proposta` — vira `Aceita` quando implementações reais chamarem a cifra, não só a biblioteca. |

As quatro formas de se referir a uma versão — wire, release, branch e biblioteca
— são coisas diferentes e não se misturam. A notação canônica de cada uma está em
[Versionamento](VERSIONING.md).

## Como ler

Quatro caminhos, dependendo do que você veio fazer:

**Vim avaliar se o BTP serve para o meu caso** — leia
[Vantagens, limites e aplicabilidade](TRADEOFFS.md). É o único capítulo que pode
ser lido isolado, e responde o que os capítulos normativos não respondem: o que
o desenho compra, o que cobra, e onde ele não cabe.

**Vou integrar o BTP em um projeto** — comece por
[O protocolo: modelo e garantias](ARCHITECTURE.md), siga para
[Do sensor à tela](WALKTHROUGH.md), leia o [Codec portátil](CODEC.md) e o
capítulo do transporte que você vai usar. Só volte à especificação normativa
quando precisar do layout exato de um campo.

**Vou implementar o protocolo em outra plataforma** (outra linguagem, outro
chip) — leia [Convenções e glossário](CONVENTIONS.md), depois
[O frame no wire](BTP_V1.md) inteiro, os payloads lógicos de
[Telemetria](TELEMETRY.md) e [Comandos](COMMANDS_AND_ACTIONS.md), o perfil de
transporte aplicável e, obrigatoriamente,
[Vetores de conformidade](CONFORMANCE.md): sua implementação não está pronta
antes de produzir e consumir os mesmos octetos dos vetores.

**Vim entender por que uma decisão é assim** — vá direto ao
[registro de decisões](decisions/README.md). Cada ADR guarda contexto,
alternativas rejeitadas e consequências; os capítulos de especificação descrevem
o que vale hoje, os ADRs descrevem por que passou a valer.

## As seis partes

| Parte | O que cobre |
| --- | --- |
| I — Panorama | O modelo de papéis e as garantias por canal; o que o desenho compra e cobra; o caminho completo de uma amostra; o vocabulário usado no resto do livro. |
| II — O contrato no wire | O frame octeto a octeto, a API do codec e o que COBS, fragmentação e reassembly fazem. |
| III — Canais lógicos | O payload de cada canal: amostras e schemas de telemetria; comandos, manifesto, sessão e terminal. |
| IV — Perfis de transporte | O que muda entre Serial, ESP-NOW e USB HID — limites, enquadramento e posse do enlace. |
| V — Criptografia | O modelo de segurança do payload cifrado, as decisões por trás dele e a API `btp::aead`. |
| VI — Processo e garantias | Os vetores canônicos, a política de versão conjunta e as regras para mudar o wire. |

O apêndice traz os ADRs.

## Escopo

O livro cobre o que duas implementações precisam concordar entre si: o wire
format, a biblioteca compartilhada e os vetores de conformidade.

Fora de escopo, por decisão: a implementação interna de qualquer consumidor,
tutorial de toolchain, e o provisionamento de identidade e de chave — que é
requisito operacional real, mas fica fora do wire (ver
[Criptografia](CRYPTO.md)).

Quando um capítulo de leitura e a especificação normativa divergirem, a
especificação vence: [O frame no wire](BTP_V1.md) é a fonte canônica, e
[Convenções e glossário](CONVENTIONS.md) explica como as palavras
**MUST**/**SHOULD** devem ser lidas nela.
