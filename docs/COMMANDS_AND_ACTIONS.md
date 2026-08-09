# BTP v1: comandos, manifesto, sessão e terminal

## 1. Convenções e escopo

As palavras **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT** e **MAY** são
requisitos normativos. Um octeto tem 8 bits. Inteiros multibyte são
serializados em little-endian. Todo campo reservado **MUST** ser zero ao
emitir e **MUST** causar rejeição quando recebido com outro valor.

Este documento especifica os payloads lógicos dos tipos `COMMAND`, `CONTROL`
e `TERMINAL` do envelope BTP v1. Envelope, CRC, identidade, sequência,
fragmentação e limites físicos continuam regidos por
[`BTP_V1.md`](BTP_V1.md). Os schemas referenciados pelo manifesto obedecem a
[`TELEMETRY.md`](TELEMETRY.md).

Os layouts abaixo descrevem o payload lógico completo, depois de eventual
reassembly. Não existe alinhamento, padding, terminador implícito ou
representação de memória de `struct`.

## 2. Primitivas comuns

Uma `utf8_u16` é `size:uint16_le` seguido de exatamente `size` octetos UTF-8,
sem BOM ou terminador. O texto **MUST** ser UTF-8 bem formado; `size=0`
representa texto vazio. Uma `bytes_u32` é `size:uint32_le` seguido de
exatamente `size` octetos arbitrários. Antes de somar ou alocar, o decoder
**MUST** validar comprimentos contra os bytes restantes e os limites da seção
13.

Uma referência completa a uma requisição é:

| Offset | Tamanho | Campo | Tipo no wire |
| ---: | ---: | --- | --- |
| 0 | 4 | `request_source_id` | `uint32_le` |
| 4 | 4 | `request_boot_id` | `uint32_le` |
| 8 | 4 | `reply_to_sequence` | `uint32_le` |

Somente `reply_to_sequence` não é globalmente único. A correlação **MUST** usar
a tripla (`request_source_id`, `request_boot_id`, `reply_to_sequence`), que é a
identidade da mensagem de requisição no envelope. Uma resposta **MUST** copiar
esses três valores exatamente.

Os códigos comuns de resultado são:

| Valor | Nome | Significado |
| ---: | --- | --- |
| `0x00` | `SUCCESS` | Operação concluída |
| `0x01` | `REJECTED` | Requisição válida, mas recusada |
| `0x02` | `FAILED` | Execução iniciada e falhou |
| `0x03` | `TIMEOUT` | Limite de execução expirou |
| `0x04` | `CANCELLED` | Execução foi cancelada |
| `0x05` | `UNSUPPORTED` | Recurso ou versão não suportado |
| `0x06` | `BUSY` | Capacidade temporariamente indisponível |
| `0x07` a `0xFF` | — | Reservados |

Os códigos comuns de erro são:

| Valor | Nome |
| ---: | --- |
| `0x0000` | `NONE` |
| `0x0001` | `MALFORMED_PAYLOAD` |
| `0x0002` | `UNKNOWN_OBJECT` |
| `0x0003` | `INVALID_ARGUMENT` |
| `0x0004` | `NOT_AUTHORIZED` |
| `0x0005` | `CAPACITY_EXHAUSTED` |
| `0x0006` | `EXECUTION_TIMEOUT` |
| `0x0007` | `INTERNAL_ERROR` |
| `0x0008` | `UNSUPPORTED_VERSION` |
| `0x0009` | `STALE_TARGET_BOOT` |
| `0x000A` | `REQUEST_CONFLICT` |
| `0x000B` | `NOT_FOUND` |
| `0x000C` a `0x7FFF` | Reservados ao BTP |
| `0x8000` a `0xFFFF` | Específicos da ação, descritos no manifesto |

`SUCCESS` **MUST** usar `error_code=NONE`. Outro status **MUST** usar um erro
diferente de `NONE`. Mensagens textuais são apenas diagnóstico humano; lógica
de cliente **MUST** depender dos códigos.

## 3. Namespaces de `object_id`

### 3.1 Mensagens `COMMAND`

| `object_id` | Nome |
| ---: | --- |
| `0x0001` | `COMMAND_REQUEST` |
| `0x0002` | `COMMAND_RESULT` |
| demais | Reservados |

### 3.2 Mensagens `CONTROL`

| `object_id` | Nome |
| ---: | --- |
| `0x0001` | `HELLO` |
| `0x0002` | `HELLO_RESULT` |
| `0x0003` | `MANIFEST_REQUEST` |
| `0x0004` | `MANIFEST_DATA` |
| `0x0005` | `SUBSCRIBE` |
| `0x0006` | `SUBSCRIBE_RESULT` |
| `0x0007` | `UNSUBSCRIBE` |
| `0x0008` | `UNSUBSCRIBE_RESULT` |
| `0x0009` | `STATUS` |
| `0x000A` | `SESSION_CLOSE` |
| `0x000B` | `SESSION_CLOSE_RESULT` |
| demais | Reservados |

### 3.3 Mensagens `TERMINAL`

| `object_id` | Nome |
| ---: | --- |
| `0x0001` | `TERMINAL_IN` |
| `0x0002` | `TERMINAL_OUT` |
| demais | Reservados |

Um receptor v1 **MUST** rejeitar `object_id` reservado sem reinterpretá-lo.

## 4. Comandos

### 4.1 `COMMAND_REQUEST`

O envelope identifica o solicitante. O payload é:

| Offset | Tamanho | Campo | Tipo no wire |
| ---: | ---: | --- | --- |
| 0 | 4 | `target_source_id` | `uint32_le` |
| 4 | 4 | `target_boot_id` | `uint32_le` |
| 8 | 2 | `action_id` | `uint16_le` |
| 10 | 2 | `action_version` | `uint16_le` |
| 12 | 2 | `flags` | `uint16_le`, zero no v1 |
| 14 | 2 | `reserved` | `uint16_le`, zero |
| 16 | 4 | `parameter_size` | `uint32_le` |
| 20 | variável | `parameters` | exatamente `parameter_size` octetos |

`target_source_id`, `target_boot_id`, `action_id` e `action_version` **MUST**
ser não zero; `parameter_size` **MAY** ser zero. `target_source_id` seleciona o
executor e `target_boot_id` fixa a inicialização contra a qual a intenção foi
criada. Um executor com outro boot **MUST NOT** executar a ação e responde
`REJECTED/STALE_TARGET_BOOT`.

`action_id`, `action_version` e o encoding de `parameters` são anunciados no
manifesto da fonte alvo. Parâmetros vazios são válidos somente quando o
manifesto declara zero campos. Parâmetros desconhecidos, truncados ou com
bytes excedentes causam `REJECTED/INVALID_ARGUMENT`.

### 4.2 `COMMAND_RESULT`

O envelope identifica o executor e usa uma `sequence` própria. O payload é:

| Offset | Tamanho | Campo | Tipo no wire |
| ---: | ---: | --- | --- |
| 0 | 12 | referência à requisição | seção 2 |
| 12 | 2 | `action_id` | `uint16_le` |
| 14 | 2 | `action_version` | `uint16_le` |
| 16 | 1 | `status` | código comum de resultado |
| 17 | 1 | `reserved` | `uint8`, zero |
| 18 | 2 | `error_code` | `uint16_le` |
| 20 | 2 + M | `message` | `utf8_u16` |
| 22 + M | 4 + R | `result` | `bytes_u32` |

`action_id` e `action_version` **MUST** repetir a requisição quando ela pôde
ser analisada; em `MALFORMED_PAYLOAD`, ambos **MAY** ser zero. O resultado usa
o encoding anunciado para a ação e **MUST** ser vazio quando não houver schema
de saída ou quando o status não for `SUCCESS`. Exatamente um resultado final é
produzido para cada requisição aceita ou rejeitada; progresso intermediário
pertence a tópicos de telemetria ou status próprios, não a
`COMMAND_RESULT`.

### 4.3 Deduplicação e idempotência

A chave de deduplicação é a identidade da mensagem `COMMAND_REQUEST`:

```text
(request_source_id, request_boot_id, request_sequence)
```

O executor **MUST** comparar também todos os bytes do payload lógico:

- primeira recepção reserva a chave antes de iniciar qualquer efeito;
- repetição da mesma chave e dos mesmos bytes nunca inicia outra execução;
- se estiver em andamento, a repetição permanece vinculada à execução
  original; depois de concluída, retransmite a mesma mensagem
  `COMMAND_RESULT`, preservando `sequence` e todos os bytes do payload lógico;
- a mesma chave com bytes diferentes é conflito, não é executada e produz
  `REJECTED/REQUEST_CONFLICT`;
- entradas de um `request_boot_id` ativo **MUST NOT** ser expulsas para aceitar
  novas requisições. Ao esgotar `max_dedup_entries`, novas chaves recebem
  `BUSY/CAPACITY_EXHAUSTED` e chaves antigas continuam protegidas.

Uma entrada aceita **MUST** permanecer no cache até terminar o boot do
executor; fechamento de sessão, desconexão de transporte, timeout, novo boot
do solicitante ou retransmissão não autorizam sua remoção antecipada. O limite
anunciado torna esse armazenamento finito: quando ele acaba, o executor recusa
novas chaves em vez de enfraquecer a garantia. Reinício do executor pode limpar
o cache sem repetir um efeito, porque todo request anterior carrega o
`target_boot_id` antigo e passa a ser rejeitado.

O manifesto marca uma ação naturalmente idempotente com `IDEMPOTENT`, mas
deduplicação continua obrigatória para todas as ações. Um solicitante **MUST**
retransmitir a mesma intenção com a mesma sequência e os mesmos bytes; usar
nova sequência cria deliberadamente outro comando.

## 5. `HELLO` e negociação

`HELLO` é a primeira mensagem BTP de uma sessão. Seu payload é:

| Offset | Tamanho | Campo | Tipo no wire |
| ---: | ---: | --- | --- |
| 0 | 1 | `role` | `uint8` |
| 1 | 1 | `version_count` | `uint8` |
| 2 | 2 | `flags` | `uint16_le`, zero no v1 |
| 4 | 4 | `max_logical_payload` | `uint32_le` |
| 8 | 2 | `max_inflight_reassemblies` | `uint16_le` |
| 10 | 2 | `max_subscriptions` | `uint16_le` |
| 12 | 4 | `max_dedup_entries` | `uint32_le` |
| 16 | 4 | `session_timeout_ms` | `uint32_le` |
| 20 | 16 | `peer_uuid` | 16 octetos |
| 36 | 4 | `config_revision` | `uint32_le` |
| 40 | V | `versions` | `version_count` valores `uint8` crescentes |

Papéis: `0x01=ROBOT`, `0x02=DONGLE`, `0x03=DESKTOP` e
`0x04=DIAGNOSTIC_TOOL`; zero e `0x05..0xFF` são reservados. UUID é uma
identidade opaca e estável de 16 octetos, comparada byte a byte, e não pode ser
toda zero; nenhuma conversão de endianness é aplicada. `config_revision=0`
significa que o peer não publica manifesto; quando há catálogo, a revisão é
monotônica e começa em 1. Capacidades e timeout devem ser não zero. No caminho que inclui ESP-NOW,
`max_logical_payload <= 53550`.

`versions` enumera as versões de envelope que o emissor pode usar. Nesta
especificação, a lista válida contém `0x01`. Entradas duplicadas, fora de ordem
ou zero invalidam o payload.

`HELLO_RESULT` contém:

| Offset | Tamanho | Campo | Tipo no wire |
| ---: | ---: | --- | --- |
| 0 | 12 | referência à requisição | seção 2 |
| 12 | 1 | `status` | `SUCCESS` ou `UNSUPPORTED` |
| 13 | 1 | `selected_version` | `uint8`; zero em falha |
| 14 | 2 | `error_code` | `uint16_le` |
| 16 | 4 | `max_logical_payload` | `uint32_le`, valor efetivo |
| 20 | 2 | `max_inflight_reassemblies` | `uint16_le`, valor efetivo |
| 22 | 2 | `max_subscriptions` | `uint16_le`, valor efetivo |
| 24 | 4 | `max_dedup_entries` | `uint32_le`, valor efetivo |
| 28 | 4 | `session_timeout_ms` | `uint32_le`, valor efetivo |
| 32 | 16 | `peer_uuid` | UUID do respondente |
| 48 | 4 | `config_revision` | revisão do respondente |

O respondente escolhe a maior versão comum que consiga usar em todos os
enlaces da sessão. Limites efetivos são o mínimo das capacidades anunciadas e
das capacidades locais. Em sucesso, todos são não zero e
`selected_version=1`. Sem interseção, responde
`UNSUPPORTED/UNSUPPORTED_VERSION`, usa versão e limites iguais a zero e fecha
a sessão depois de transmitir a resposta. Nenhuma outra mensagem pode ser
enviada antes do `HELLO_RESULT` bem-sucedido.

## 6. Manifesto e descoberta

Cada `MANIFEST_DATA` descreve exatamente uma fonte. O envelope identifica quem
respondeu à requisição, enquanto `described_source_id` e
`described_boot_id` identificam a fonte catalogada. Um gateway pode responder
de seu cache, mas **MUST** preservar esses IDs e o UUID do produtor. Uma
requisição com alvo zero enumera o catálogo inteiro e permite que um cliente
sem configuração prévia descubra as fontes.

### 6.1 `MANIFEST_REQUEST`

| Offset | Tamanho | Campo | Tipo no wire |
| ---: | ---: | --- | --- |
| 0 | 4 | `target_source_id` | `uint32_le`; zero solicita o catálogo inteiro |
| 4 | 4 | `target_boot_id` | `uint32_le`; zero aceita o boot atual |
| 8 | 4 | `known_config_revision` | `uint32_le`; zero força conteúdo completo |

Com alvo não zero, a fonte produz uma resposta. Se ela não existir, o gateway
**MUST** produzir `MANIFEST_DATA` com `NOT_FOUND`. Se `target_boot_id` não for
zero e divergir, produz `STALE_TARGET_BOOT`. Uma revisão conhecida igual à
atual permite resposta `NOT_MODIFIED` sem descritores.

Com `target_source_id=0`, `target_boot_id` e `known_config_revision` **MUST**
ser zero. O gateway tira um snapshot das fontes conhecidas, incluindo a si
próprio, ordena-as por `source_id` e produz uma resposta completa para cada
uma. Assim, `catalog_count` é sempre ao menos um. Entradas que mudarem durante
a enumeração aparecem somente em uma requisição posterior; o snapshot não
pode mudar no meio da resposta.

### 6.2 `MANIFEST_DATA`

O prefixo fixo é:

| Offset | Tamanho | Campo | Tipo no wire |
| ---: | ---: | --- | --- |
| 0 | 12 | referência à requisição | seção 2 |
| 12 | 1 | `status` | `SUCCESS`, `REJECTED` ou `UNSUPPORTED` |
| 13 | 1 | `flags` | bit `0`: `NOT_MODIFIED`; bit `1`: `CATALOG_COMPLETE` |
| 14 | 2 | `error_code` | `uint16_le` |
| 16 | 2 | `manifest_format_version` | `uint16_le`, valor 1 |
| 18 | 2 | `reserved` | `uint16_le`, zero |
| 20 | 4 | `config_revision` | `uint32_le` |
| 24 | 16 | `source_uuid` | 16 octetos |
| 40 | 4 | `described_source_id` | `uint32_le` |
| 44 | 4 | `described_boot_id` | `uint32_le` |
| 48 | 1 | `source_role` | `uint8`, mesmos códigos de `HELLO` |
| 49 | 1 | `source_flags` | bit 0 `ONLINE`; demais zero |
| 50 | 2 | `catalog_index` | `uint16_le`, começando em zero |
| 52 | 2 | `catalog_count` | `uint16_le` |
| 54 | 2 | `topic_count` | `uint16_le` |
| 56 | 2 | `action_count` | `uint16_le` |
| 58 | 2 + N | `source_name` | `utf8_u16` |
| variável | variável | tópicos | `topic_count` registros |
| variável | variável | ações | `action_count` registros |

Em requisição direcionada, `catalog_index=0`, `catalog_count=1` e
`CATALOG_COMPLETE` está marcado. Em enumeração, todas as respostas repetem o
mesmo `catalog_count`, usam índices contíguos e somente a última marca
`CATALOG_COMPLETE`. Uma resposta ausente ou repetida torna o snapshot
incompleto; o cliente não publica o novo catálogo e pode refazer a requisição.

Em `SUCCESS`, os IDs descritos, revisão, UUID e nome são válidos e todos os
registros seguem na ordem crescente de ID. Com `NOT_MODIFIED`,
`status=SUCCESS`, identidade e nome continuam presentes, contagens de tópico e
ação são zero e não há registros; essa flag só é válida em requisição
direcionada. `ONLINE` indica que o gateway possui uma sessão atual com a fonte;
fontes conhecidas em cache podem ser anunciadas offline. Em erro, IDs
descritos, papel, flags, contagens e revisão são zero, UUID é todo zero,
`catalog_index=0`, `catalog_count=1` e o nome é a mensagem humana opcional;
`CATALOG_COMPLETE` está marcado e não há registros.

Cada registro começa com `record_size:uint32_le`, que conta os bytes depois do
próprio tamanho. O decoder **MUST** limitar a leitura ao registro, consumir
exatamente `record_size` e rejeitar o manifesto inteiro em qualquer
inconsistência. O v1 não ignora sufixos desconhecidos.

Um registro de tópico contém, nesta ordem:

| Campo | Tipo no wire |
| --- | --- |
| `record_size` | `uint32_le` |
| `topic_id`, `schema_version` | dois `uint16_le` não zero |
| `encoding` | `uint8`, códigos de `TELEMETRY.md` |
| `flags` | `uint8`; bit 0 `SUBSCRIBABLE`, demais zero |
| `field_count` | `uint16_le` |
| `max_rate_millihz` | `uint32_le`; zero significa não periódico |
| `name`, `description` | duas `utf8_u16` |
| `fields` | `field_count` registros de campo |

Cada registro de campo contém:

| Campo | Tipo no wire |
| --- | --- |
| `record_size` | `uint32_le` |
| `field_id`, `order` | dois `uint16_le` |
| `type` | `uint8`, códigos abaixo |
| `flags` | `uint8`; bit 0 `NULLABLE`, bit 1 `VARIABLE_COUNT` |
| `element_count` | `uint16_le`; contagem fixa ou zero quando variável |
| `max_element_count` | `uint16_le`; máximo variável ou zero quando fixo |
| `scale`, `offset` | dois IEEE-754 `float64` little-endian finitos |
| `enum_count` | `uint16_le` |
| `name`, `unit`, `description` | três `utf8_u16` |
| `enum_entries` | `enum_count` entradas (`value:uint16_le`, `label:utf8_u16`) |

Códigos de tipo são, na ordem de `TELEMETRY.md`: `0x01=uint8`,
`0x02=uint16`, `0x03=uint32`, `0x04=uint64`, `0x05=int8`, `0x06=int16`,
`0x07=int32`, `0x08=int64`, `0x09=float32`, `0x0A=float64`, `0x0B=bool`,
`0x0C=enum8` e `0x0D=enum16`. Zero e valores maiores são reservados.
`enum_count` é zero salvo para enums; entradas são crescentes e cabem na
largura declarada. As demais invariantes de campo são as de `TELEMETRY.md`.

Como exceção exclusiva para tópicos integrais `OPAQUE_BYTES` e `UTF8`, os
códigos de descritor `0x0E=opaque_bytes` e `0x0F=utf8` representam o único
`field_id` lógico opcional permitido por `TELEMETRY.md`. Nesse caso,
`field_count` é zero ou um; havendo campo, `order=0`, flags e contagens são
zero, `scale=1`, `offset=0`, `enum_count=0` e o código deve corresponder ao
encoding do tópico. Esses códigos não são válidos em tópico estruturado nem
em schema de ação.

Um registro de ação contém:

| Campo | Tipo no wire |
| --- | --- |
| `record_size` | `uint32_le` |
| `action_id`, `action_version` | dois `uint16_le` não zero |
| `flags` | `uint16_le`; bit 0 `IDEMPOTENT`, bit 1 `DANGEROUS` |
| `parameter_encoding`, `result_encoding` | dois `uint8` |
| `parameter_field_count`, `result_field_count` | dois `uint16_le` |
| `execution_timeout_ms` | `uint32_le` não zero |
| `name`, `description`, `confirmation_text` | três `utf8_u16` |
| `parameter_fields` | registros de campo em `order` crescente |
| `result_fields` | registros de campo em `order` crescente |
| `error_count` | `uint16_le` |
| `errors` | entradas (`error_code:uint16_le`, `label:utf8_u16`) crescentes |

Encodings de ação permitidos são `0x00=EMPTY`, `0x05=PACKED_LE` e
`0x06=TLV_LE`. `EMPTY` exige contagem zero; os outros usam exatamente as
regras de campo e encoding de `TELEMETRY.md`, sem prefixo de
`schema_version`. Erros específicos ficam entre `0x8000` e `0xFFFF`. A flag
`DANGEROUS` exige confirmação explícita do usuário, mas não autoriza o cliente
a inventar semântica local; `confirmation_text` vem da fonte e **MUST** ser
não vazio quando essa flag estiver marcada.

IDs e versões são estáveis. Qualquer mudança de interpretação de tópico,
campo, parâmetros, resultado ou ação exige incremento da respectiva versão e
de `config_revision`. Uma revisão nunca é reutilizada para conteúdo diferente.

## 7. Assinaturas

`SUBSCRIBE` solicita publicação periódica de um tópico:

| Offset | Tamanho | Campo | Tipo no wire |
| ---: | ---: | --- | --- |
| 0 | 4 | `target_source_id` | `uint32_le` |
| 4 | 4 | `target_boot_id` | `uint32_le` não zero |
| 8 | 2 | `topic_id` | `uint16_le` não zero |
| 10 | 2 | `flags` | zero no v1 |
| 12 | 4 | `requested_rate_millihz` | `uint32_le` não zero |
| 16 | 4 | `requested_lease_ms` | `uint32_le` não zero |

`SUBSCRIBE_RESULT` contém referência à requisição (12 octetos),
`status:uint8`, `reserved:uint8`, `error_code:uint16_le`,
`subscription_id:uint32_le`, `effective_rate_millihz:uint32_le` e
`granted_lease_ms:uint32_le`. Em sucesso, os três últimos são não zero. Em
erro, são zero. A taxa efetiva **MUST NOT** exceder a solicitada nem a máxima
do manifesto. A publicação pode ter jitter e perdas; telemetria continua best
effort e não recebe ACK por amostra.

Repetir a mesma requisição, com a mesma identidade e bytes, retorna a mesma
assinatura sem criar outra. Uma nova sequência cria ou substitui, de maneira
atômica, a assinatura da mesma sessão e do mesmo tópico. A assinatura expira
após o lease se não for renovada por novo `SUBSCRIBE`.

`UNSUBSCRIBE` contém `target_source_id:uint32_le`,
`target_boot_id:uint32_le` e `subscription_id:uint32_le`. Seu resultado contém
referência à requisição, `status:uint8`, `reserved:uint8` e
`error_code:uint16_le`. Remover uma assinatura já ausente retorna
`SUCCESS/NONE`, tornando retries idempotentes.

## 8. Status

`STATUS` é publicação espontânea e não possui resposta. `source_id` e
`boot_id` do envelope definem o escopo dos contadores. O payload v1 fixo é:

| Offset | Tamanho | Campo | Tipo no wire |
| ---: | ---: | --- | --- |
| 0 | 2 | `status_version` | `uint16_le`, valor 1 |
| 2 | 2 | `flags` | bit 0 `DEGRADED`, demais zero |
| 4 | 8 | `uptime_us` | `uint64_le` |
| 12 | 8 | `frames_rx` | `uint64_le` |
| 20 | 8 | `frames_tx` | `uint64_le` |
| 28 | 8 | `frames_dropped` | `uint64_le` |
| 36 | 8 | `crc_errors` | `uint64_le` |
| 44 | 8 | `decode_errors` | `uint64_le` |
| 52 | 8 | `reassembly_completed` | `uint64_le` |
| 60 | 8 | `reassembly_timeouts` | `uint64_le` |
| 68 | 8 | `reassembly_rejected` | `uint64_le` |
| 76 | 8 | `command_duplicates` | `uint64_le` |
| 84 | 8 | `telemetry_dropped` | `uint64_le` |

O tamanho lógico é exatamente 92 octetos. Contadores são monotônicos e
saturam em `UINT64_MAX`; reiniciam somente com novo `boot_id`.
`frames_dropped` inclui frames válidos descartados por filas ou capacidade;
`crc_errors` conta frames rejeitados por CRC; `decode_errors`, envelopes ou
payloads malformados; e contadores de reassembly separam sucesso, timeout e
inconsistência/capacidade. Um mesmo evento **MAY** contribuir para um contador
específico e para `frames_dropped`, mas não pode ser contado duas vezes no
mesmo contador.

## 9. Terminal opaco

O payload lógico inteiro de `TERMINAL_IN` e `TERMINAL_OUT` é um bloco de bytes
opacos delimitado por `payload_size` ou pelo tamanho reassemblado. Zero, CR,
LF, UTF-8 inválido e sequências que parecem frames são dados válidos. Não há
prefixo, terminador ou exigência de texto.

`TERMINAL_IN` flui do controlador da sessão para a entrada do terminal da
fonte; `TERMINAL_OUT` flui no sentido inverso. Fragmentos só são entregues
depois do reassembly. Entre mensagens completas de uma mesma origem, o
consumidor usa `sequence` para ordenar e auxiliar a detecção de perdas. Como a
sequência é compartilhada por todos os tipos, uma lacuna pode pertencer a
outro canal; o terminal **MUST NOT** fabricar ou preencher bytes ausentes.

Terminal e telemetria permanecem semanticamente separados. Um terminal
**MUST NOT** analisar, exibir ou aguardar um frame `TELEMETRY`, e bytes de
terminal **MUST NOT** ser publicados como telemetria ou log.

## 10. Entrada e saída do modo protocolado serial

No modo console, o dongle reconhece somente uma linha ASCII completa com esta
forma, em que `NNNNNNNNNNNNNNNN` são 16 dígitos hexadecimais minúsculos ou
maiúsculos escolhidos pelo cliente:

```text
BTP/1 ENTER NNNNNNNNNNNNNNNN\r\n
```

Ele responde, repetindo o nonce com letras minúsculas:

```text
BTP/1 READY nnnnnnnnnnnnnnnn\r\n
```

Somente depois de transmitir `READY` ambos passam ao framing binário. O
cliente envia `HELLO` em até 2000 ms e nenhuma outra mensagem antes dele.
Linha inválida ou incompleta permanece entrada comum do console e nunca ativa
autodetecção binária.

O modo protocolado termina por uma troca BTP, nunca procurando texto ou uma
sequência de escape dentro dos bytes codificados. `SESSION_CLOSE` tem payload
`reason:uint8`, três octetos reservados e `drain_timeout_ms:uint32_le`. Razões:
`0x00=NORMAL`, `0x01=VERSION_MISMATCH`, `0x02=CLIENT_SHUTDOWN` e
`0x03=PROTOCOL_ERROR`. `SESSION_CLOSE_RESULT` contém a referência à requisição,
`status:uint8`, `reserved:uint8` e `error_code:uint16_le`.

Ao receber um fechamento válido, o dongle para de aceitar novo trabalho,
aguarda no máximo `min(drain_timeout_ms, 2000)` ms para transmitir o resultado,
descarta reassemblies incompletos e só então volta ao console. Após a transição
ele emite exatamente `BTP/1 CONSOLE\r\n`. Se nenhum frame BTP válido for
recebido durante `session_timeout_ms`, ou se `HELLO` não chegar no prazo, o
dongle faz a mesma limpeza e retorna ao console. Tráfego inválido não renova o
watchdog. Perder o transporte não autoriza repetir comandos e não apaga o
cache de deduplicação.

Os detalhes de COBS, delimitadores, decoder incremental e tamanho do buffer
são responsabilidade da especificação do transporte serial.

## 11. Timeouts e incompatibilidade

O emissor **MUST** concluir reassembly e cada operação dentro dos limites
anunciados. O timeout de execução de uma ação vem do manifesto; ao expirar, o
executor produz `TIMEOUT/EXECUTION_TIMEOUT` e impede que a ação continue
gerando efeitos. Se isso não puder ser garantido, a ação **MUST NOT** ser
anunciada como comando síncrono BTP v1.

Payload, versão de manifesto, encoding, tipo, ação ou campo desconhecido é
rejeitado explicitamente. Não existe parser legado, fallback textual,
autodetecção ou tentativa com outro schema. Um erro em mensagem fragmentada é
reportado somente depois de identificar com segurança a mensagem e nunca
entrega conteúdo parcial.

Uma mudança em `config_revision` invalida descritores em cache. O cliente
**SHOULD** pausar decodificação de schemas desconhecidos e solicitar novo
manifesto; não deve adivinhar pela revisão anterior.

## 12. Prioridade lógica e congestionamento

Implementações **MUST** usar filas lógicas separadas e aplicar esta ordem,
preservando FIFO dentro de cada classe:

1. sessão (`HELLO`, fechamento) e `COMMAND_RESULT`;
2. `COMMAND_REQUEST` e resultados de controle;
3. controle de assinatura e status com `DEGRADED`;
4. `TERMINAL_IN` e `TERMINAL_OUT`;
5. `MANIFEST_DATA`, status periódico e `LOG`;
6. `TELEMETRY`.

Prioridade não muda `sequence`, não permite interromper um frame já iniciado
e não transforma transporte best effort em confiável. Um emissor **MUST**
reservar capacidade para ao menos uma mensagem das classes 1 e 2. Sob pressão,
descarta primeiro telemetria, depois logs/status periódicos e contabiliza a
perda; manifesto grande e terminal **MUST** ser fatiados/agendados para não
impedir comandos. Uma implementação **SHOULD** oferecer progresso limitado às
classes inferiores quando houver capacidade, sem atrasar os limites de
comando e sessão.

## 13. Limites e validação

Para o BTP v1:

| Limite | Valor |
| --- | ---: |
| payload lógico em caminho com ESP-NOW | 53550 octetos (`255 * 210`) |
| manifesto lógico | 49152 octetos |
| texto `utf8_u16` individual | 1024 octetos |
| nome ou unidade | 128 octetos |
| mensagem de resultado | 512 octetos |
| parâmetros ou resultado de ação | 32768 octetos |
| campos por tópico ou lado de uma ação | 256 |
| fontes por catálogo | 1024 |
| tópicos por manifesto | 1024 |
| ações por manifesto | 1024 |
| entradas de enum ou erro por registro | 256 |

O limite efetivo é sempre o menor entre esta tabela, o `HELLO_RESULT`, o
manifesto e o transporte. Um produtor **MUST NOT** anunciar um descritor cujo
tamanho máximo viole o caminho negociado.

Depois de validar o envelope e concluir o reassembly, o receptor **MUST**:

1. resolver `type` e `object_id` sem fallback;
2. exigir o tamanho mínimo fixo e validar reservados;
3. validar cada comprimento antes de ler ou alocar;
4. validar IDs, versões, códigos, flags, contagens e limites;
5. exigir consumo exato do payload lógico;
6. somente então alterar sessão, executar ação, publicar catálogo ou entregar
   bytes ao terminal.

Manifesto e respostas de comando que excedam um frame usam a fragmentação
comum de `BTP_V1.md`; não possuem fragmentação interna alternativa.
