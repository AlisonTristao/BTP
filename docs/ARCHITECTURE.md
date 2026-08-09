# Arquitetura do BTP

## Contexto

O BTP conecta três domínios com responsabilidades deliberadamente separadas:

```text
bally_software / bally_OS       t_dongle_develop                TraceView
-------------------------       -----------------                ---------
sensores e atuadores      BTP   gateway ESP-NOW <-> USB   BTP   apresentação
origem do timestamp       --->  roteamento por canal       --->  e interação
telemetria e logs         <---  catálogo e ações           <---  do usuário
execução de comandos            persistidas
```

O dongle encaminha envelopes BTP sem converter payloads binários em texto. O
transporte muda entre os enlaces, mas a identidade da fonte, o tópico, o
timestamp de origem e o payload não ganham uma nova semântica no gateway.

## Responsabilidades por componente

### `bally_software` (`bally_OS`)

O firmware do robô é responsável por:

- adquirir dados e publicá-los em tópicos de telemetria;
- criar o timestamp na origem da amostra;
- emitir eventos e diagnósticos pelo canal de log;
- receber comandos, executar a semântica de domínio e produzir resultados;
- respeitar assinaturas e limites de taxa quando esses recursos forem
  introduzidos.

#### Logger e TelemetryPublisher

`Logger` e `TelemetryPublisher` possuem propósitos diferentes e não devem ser
fundidos:

| Componente | Uso | Característica |
| --- | --- | --- |
| `Logger` | Eventos, falhas e mensagens de diagnóstico | Esporádico, orientado a texto/evento, canal `LOG`. |
| `TelemetryPublisher` | Amostras de sensores e estado periódico | Frequente, estruturado por schema, preferencialmente `PACKED_LE`, canal `TELEMETRY`. |

Enviar amostras frequentes pelo `Logger` perde a identidade estruturada dos
campos, mistura canais e dificulta controle de taxa. O publisher não substitui
o logger: eventos continuam sendo eventos, mesmo quando contêm números.

### `t_dongle_develop`

O dongle é o gateway entre ESP-NOW e USB Serial. Ele é responsável por:

- validar envelopes e rotear os canais lógicos;
- preservar o timestamp e a identidade da origem;
- aplicar o framing apropriado a cada transporte sem interpretar como texto
  um payload binário;
- apresentar ao computador o catálogo de fontes, tópicos e ações;
- persistir a definição das ações virtuais e ser o dono desse catálogo;
- correlacionar comandos e resultados, incluindo deduplicação;
- controlar sessão, filas e políticas de entrega do enlace.

O dongle não redefine schemas de telemetria e não substitui timestamps do
robô. Persistir uma ação significa guardar a intenção configurada e os dados
necessários para executá-la; a semântica final do comando continua no produtor
que o executa.

### `TraceView`

O TraceView é a camada de apresentação e interação. Ele é responsável por:

- descobrir o manifesto exposto pelo dongle;
- associar visualizações a `source + topic + field`;
- decodificar dados usando o schema e a versão canônicos;
- plotar pelo timestamp criado na origem;
- solicitar assinaturas e taxas;
- apresentar ações descobertas e enviar seus parâmetros;
- oferecer terminal protocolado sem misturar o tráfego com logs ou
  telemetria.

O TraceView não cria IDs de telemetria por gráfico e não mantém semântica local
dos comandos. Rótulos, layout e preferências visuais podem ser locais; o que
uma ação faz, quais parâmetros aceita e como é identificada vêm do catálogo do
dongle e do contrato BTP.

## Canais lógicos

| Canal | Direção típica | Semântica |
| --- | --- | --- |
| `TELEMETRY` | robô -> PC | Amostras best effort, identificadas por fonte/tópico e schema. |
| `LOG` | robô/dongle -> PC | Eventos e diagnóstico; não substitui telemetria. |
| `COMMAND` | PC/dongle <-> robô | Requisição com request ID e resultado correlacionado; sujeita a deduplicação. |
| `TERMINAL` | bidirecional | Sessão de terminal isolada dos demais canais. |

Uma amostra de telemetria não recebe ACK individual. Resultados de comandos
possuem correlação porque representam operações, não confirmação de cada dado
best effort.

## Fluxos principais

### Telemetria

1. O `TelemetryPublisher` cria a amostra, timestamp e payload segundo o schema.
2. O enlace ESP-NOW transporta o frame BTP até o dongle.
3. O dongle valida e encaminha o frame pelo canal de telemetria.
4. A serial protocolada enquadra bytes BTP; payloads não viram linhas de texto.
5. O TraceView seleciona o schema por fonte/tópico, decodifica os campos e os
   apresenta pelo timestamp da origem.

### Comando

1. O TraceView seleciona uma ação anunciada pelo catálogo do dongle.
2. Uma requisição com request ID segue pelo canal `COMMAND`.
3. O dongle encaminha ou executa a ação persistida conforme sua definição.
4. O executor deduplica requisições repetidas e produz um resultado
   correlacionado.
5. O TraceView apresenta o estado; não reimplementa a ação localmente.

### Log e terminal

Logs são eventos unidirecionais do canal `LOG`. O terminal é uma sessão
bidirecional própria no canal `TERMINAL`. A USB Serial também poderá oferecer
um modo de console humano separado do modo protocolado; texto humano nunca é
inferido a partir de um payload BTP arbitrário.

## Envelope, payload e transportes

O envelope BTP fornece identificação e tamanho. O payload é uma sequência
opaca que pode conter qualquer byte, inclusive `0x00`, `0x0A` e `0x0D`.
Separadores de linha não delimitam payloads.

Os valores usam larguras fixas e little-endian. Nenhum componente transmite a
representação de memória de uma `struct`, pois padding, alinhamento, ABI e
endianness variam entre plataformas. A identidade, os encodings, os tipos e as
regras de arrays de telemetria estão em [`TELEMETRY.md`](TELEMETRY.md).
Os layouts e as garantias de comandos, manifesto, assinatura, sessão, status e
terminal estão em
[`COMMANDS_AND_ACTIONS.md`](COMMANDS_AND_ACTIONS.md).

ESP-NOW e USB Serial são transportes, não versões alternativas da semântica do
BTP. O envelope, o CRC, os limites e as invariantes de fragmentação estão em
[`BTP_V1.md`](BTP_V1.md). A serial protocolada usará frames BTP delimitados por
COBS; as regras operacionais de cada transporte e de reassembly pertencem aos
tópicos próprios.

## Compatibilidade

Não há fallback, autodetecção nem adaptador para o protocolo legado. Um peer
incompatível deve ser rejeitado de forma explícita conforme as regras de
[sessão e negociação](COMMANDS_AND_ACTIONS.md).

Especificação, implementação compartilhada e vetores de conformidade existem
somente neste repositório. Consulte a [política de
versionamento](VERSIONING.md) e o [registro de decisões](decisions/README.md).

## Estado das decisões do contrato

O layout, os campos, o CRC e os limites do envelope estão congelados em
[`BTP_V1.md`](BTP_V1.md). Permanecem para os próximos tópicos:

- regras operacionais de transporte, COBS e reassembly;
- formato de distribuição da futura biblioteca compartilhada.
