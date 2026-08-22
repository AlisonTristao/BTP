# O protocolo: modelo e garantias

## Contexto

O BTP move telemetria, logs, comandos e terminal entre um produtor embarcado e
um consumidor, atravessando enlaces com características opostas. O que muda de
um enlace para o outro é o enquadramento e o limite de tamanho; o que **não**
muda é a semântica: a identidade da fonte, o tópico, o instante de origem e o
payload não ganham significado novo em nenhum ponto do caminho.

Este capítulo descreve o modelo — quais papéis existem, o que o protocolo exige
de cada um, e quais garantias cada canal oferece. Os layouts normativos vivem em
[`BTP_V1.md`](BTP_V1.md), [`TELEMETRY.md`](TELEMETRY.md) e
[`COMMANDS_AND_ACTIONS.md`](COMMANDS_AND_ACTIONS.md).

## Papéis

O BTP define **papéis**, não componentes. Um mesmo processo pode acumular mais
de um papel, e um papel pode ser desempenhado por qualquer implementação que
cumpra as regras abaixo.

### Produtor

Origina mensagens. É responsável por:

- adquirir dados e publicá-los em tópicos de telemetria segundo um schema
  declarado;
- **criar o `timestamp_us` no instante da amostra**, não no envio;
- manter `source_id` estável e `boot_id` distinto por inicialização;
- emitir eventos e diagnóstico pelo canal de log, separados da telemetria;
- receber comandos, executar a semântica de domínio, deduplicar requisições
  repetidas e produzir resultados correlacionados;
- respeitar assinaturas e limites de taxa quando anunciar tópicos assináveis.

Um produtor não reinterpreta um `schema_version` já emitido: mudança de campo,
tipo, ordem, unidade, escala ou encoding exige versão nova.

### Gateway (opcional)

Encaminha mensagens entre dois enlaces. Um caminho BTP pode não ter gateway
nenhum, ou ter mais de um. É responsável por:

- validar o envelope antes de encaminhar e rotear pelos canais lógicos;
- **preservar `source_id`, `boot_id`, `sequence` e `timestamp_us` intactos**;
- aplicar o enquadramento de cada transporte sem interpretar payload binário
  como texto;
- reenquadrar entre perfis quando os limites diferirem, sem alterar a mensagem
  lógica;
- controlar sessão, filas e política de entrega de cada enlace que possui.

Um gateway não redefine schema de telemetria e não substitui timestamp de
origem — [`BTP_V1.md`](BTP_V1.md) §6 proíbe as duas coisas. Ele é gateway, não
tradutor.

Quando também expõe um catálogo próprio de fontes, tópicos e ações, o gateway
passa a ser produtor daquele catálogo e assume as obrigações de produtor sobre
ele: persistir a definição, ser dono da revisão e correlacionar comandos que ele
mesmo executa. A semântica final de um comando continua com quem o executa.

### Consumidor

Decodifica e apresenta. É responsável por:

- descobrir o manifesto e associar leituras a `source + topic + field`;
- decodificar usando o schema e a versão canônicos, sem adivinhar versão
  desconhecida;
- ordenar e apresentar pelo `timestamp_us` criado na origem;
- solicitar assinaturas e taxas em vez de presumir publicação;
- enviar parâmetros de ações descobertas no catálogo, sem reimplementar a
  semântica localmente;
- manter o terminal como sessão isolada, sem misturar seu tráfego com log ou
  telemetria.

Rótulo, cor, layout e preferência visual são locais. O que uma ação faz, quais
parâmetros aceita e como é identificada vêm do catálogo e do contrato.

### Topologia de referência

Não normativa — serve só para ancorar a leitura dos capítulos de transporte:

```text
produtor              gateway               consumidor
--------              -------               ----------
amostra + schema
timestamp de origem
                      valida e roteia
   ---- rádio ---->   reenquadra
                         ---- barramento local ---->   decodifica
                                                       apresenta
   <---------------- comando -------------------------------
```

O número de saltos, os transportes de cada salto e a existência de gateway são
propriedades do deploy, não do protocolo.

## Canais lógicos

| Canal | Direção típica | Semântica |
| --- | --- | --- |
| `TELEMETRY` | produtor -> consumidor | Amostras best effort, identificadas por fonte/tópico e schema. |
| `LOG` | produtor/gateway -> consumidor | Eventos e diagnóstico; não substitui telemetria. |
| `COMMAND` | consumidor/gateway <-> produtor | Requisição com request ID e resultado correlacionado; sujeita a deduplicação. |
| `TERMINAL` | bidirecional | Sessão de terminal isolada dos demais canais. |
| `CONTROL` | bidirecional | Sessão, descoberta, manifesto, assinatura e status. |

Uma amostra de telemetria não recebe ACK individual. Resultados de comando
possuem correlação porque representam operações, não confirmação de cada dado
best effort.

**Log e telemetria não são intercambiáveis.** Enviar amostras frequentes pelo
canal de log perde a identidade estruturada dos campos, mistura canais e
dificulta controle de taxa; publicar um evento como telemetria força um schema
onde não há um. Evento continua sendo evento, mesmo quando contém números.

A ordem de prioridade entre canais, as filas separadas e a política de descarte
sob pressão estão em [`COMMANDS_AND_ACTIONS.md`](COMMANDS_AND_ACTIONS.md) §12.

## Fluxos principais

### Telemetria

1. O produtor monta a amostra com timestamp e payload segundo o schema.
2. O enlace transporta o frame BTP.
3. Um gateway, se houver, valida e encaminha pelo canal de telemetria.
4. O enlace seguinte reenquadra os mesmos octetos BTP; payload binário não vira
   linha de texto em nenhum ponto.
5. O consumidor resolve o schema por fonte/tópico, decodifica os campos e os
   apresenta pelo timestamp da origem.

O caminho completo, etapa por etapa e com os números do exemplo, está em
[Do sensor à tela](WALKTHROUGH.md).

### Comando

1. O consumidor escolhe uma ação anunciada pelo catálogo.
2. Uma requisição com request ID segue pelo canal `COMMAND`.
3. Um gateway encaminha, ou executa a ação de que ele é dono.
4. O executor deduplica requisições repetidas e produz um resultado
   correlacionado.
5. O consumidor apresenta o estado; não reimplementa a ação localmente.

### Log e terminal

Logs são eventos unidirecionais do canal `LOG`. O terminal é uma sessão
bidirecional própria no canal `TERMINAL`, cujos bytes são opacos. Quando o
transporte oferece um modo de console humano, ele é separado do modo protocolado
e a alternância é definida em [`TRANSPORT_SERIAL.md`](TRANSPORT_SERIAL.md);
texto humano nunca é inferido a partir de um payload BTP arbitrário.

## Envelope, payload e transportes

O envelope BTP fornece identificação e tamanho. O payload é uma sequência opaca
que pode conter qualquer octeto, inclusive `0x00`, `0x0A` e `0x0D`. Separadores
de linha não delimitam payloads.

Os valores usam larguras fixas e little-endian. Nenhum componente transmite a
representação de memória de uma `struct`, pois padding, alinhamento, ABI e
endianness variam entre plataformas. A identidade, os encodings, os tipos e as
regras de arrays de telemetria estão em [`TELEMETRY.md`](TELEMETRY.md). Os
layouts e as garantias de comandos, manifesto, assinatura, sessão, status e
terminal estão em [`COMMANDS_AND_ACTIONS.md`](COMMANDS_AND_ACTIONS.md).

ESP-NOW, USB Serial e USB HID são transportes, não versões alternativas da
semântica do BTP. O envelope, o CRC, os limites e as invariantes de fragmentação
estão em [`BTP_V1.md`](BTP_V1.md). Um frame ou fragmento ocupa um datagrama
conforme [`TRANSPORT_ESPNOW.md`](TRANSPORT_ESPNOW.md); a serial protocolada usa
COBS, decoder incremental e propriedade exclusiva da porta conforme
[`TRANSPORT_SERIAL.md`](TRANSPORT_SERIAL.md); a interface HID entrega um
relatório de tamanho fixo por vez conforme
[`TRANSPORT_USB_HID.md`](TRANSPORT_USB_HID.md). O reassembly pertence à
implementação compartilhada.

O que cada perfil compra e cobra está resumido em
[Vantagens, limites e aplicabilidade](TRADEOFFS.md).

## Compatibilidade

Não há fallback, autodetecção nem adaptador para formato anterior. Um peer
incompatível deve ser rejeitado de forma explícita conforme as regras de
[sessão e negociação](COMMANDS_AND_ACTIONS.md).

Especificação, implementação compartilhada e vetores de conformidade existem
somente neste repositório. Consulte a
[política de versionamento](VERSIONING.md) e o
[registro de decisões](decisions/README.md).

## Estado das decisões do contrato

O layout, os campos, o CRC e os limites do envelope estão congelados em
[`BTP_V1.md`](BTP_V1.md), e as regras operacionais dos transportes estão nos
documentos ESP-NOW, Serial e USB HID. Os utilitários compartilhados de COBS,
decoder incremental, fragmentação e reassembly estão implementados e
documentados em [`STREAM_AND_REASSEMBLY.md`](STREAM_AND_REASSEMBLY.md). Os
vetores canônicos existem para o wire v1 e para a extensão AEAD do wire v2, com
o contrato dos arquivos em [`CONFORMANCE.md`](CONFORMANCE.md).

O codec do envelope já é compartilhado por CMake e PlatformIO, conforme
[`CODEC.md`](CODEC.md).

A criptografia AEAD do payload ([`CRYPTO.md`](CRYPTO.md)) está especificada e
implementada na biblioteca, e a decisão que a introduz segue em estado
`Proposta`: enquanto nenhuma implementação chamar cifra de verdade, o tráfego
segue com `ENCRYPTED` limpo.
