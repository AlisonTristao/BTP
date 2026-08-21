# Do sensor à tela

Este capítulo segue uma amostra de telemetria do momento em que o firmware a
produz até o instante em que ela aparece em um gráfico, e depois faz o
caminho inverso com um comando. Nada aqui é normativo: é o mapa que amarra os
capítulos seguintes, com um link em cada etapa para o documento que define as
regras dela.

O exemplo é o tópico `protocol.test`, o mesmo da
[captura de hardware](integration-captures/README.md) guardada no
repositório: dois campos, `counter` (`uint32`) e `value` (`float32`), em
`PACKED_LE`, publicados a 50 Hz.

```text
bally_OS (robô)              bally_dongle (gateway)          TraceView (PC)
-----------------            ----------------------          --------------
1 amostra + schema
2 envelope
3 (cifra opcional)
4 fragmentação
5 CRC + serialização
   ---- ESP-NOW ---->        6 valida e roteia
                            7 framing do enlace USB
                               ---- Serial/COBS ou HID ---->  8 decode
                                                             9 reassembly
                                                            10 tag AEAD
                                                            11 schema
                                                            12 plot
```

## 1. A amostra e o schema

O `TelemetryPublisher` monta o corpo da amostra segundo o schema do tópico:
os dois campos em ordem declarada, larguras fixas, little-endian, sem nome de
campo nem unidade dentro da amostra. O payload lógico é
`schema_version (uint16_le) ‖ encoded_body` — no exemplo, 2 + 8 = **10
octetos**.

Nomes, tipos, unidades, escala e offset são metadados do schema, anunciados
uma vez pelo manifesto e nunca repetidos em cada amostra: é a tripla
(`source_id`, `topic_id`, `schema_version`) que permite ao outro lado
decodificar esses 10 octetos. Um campo novo, uma unidade diferente ou uma
mudança de ordem exigem um `schema_version` novo, jamais reinterpretar um já
emitido.

→ [Telemetria, arrays e schemas](TELEMETRY.md)

## 2. O envelope

O payload ganha um header de 36 octetos que responde quem, quando e o quê:

| Campo | Valor neste exemplo |
| --- | --- |
| `type` | `TELEMETRY` (`0x01`) |
| `object_id` | o `topic_id` de `protocol.test` |
| `source_id` | identidade estável do robô, não nula |
| `boot_id` | identidade desta inicialização |
| `sequence` | identifica esta amostra dentro do boot |
| `timestamp_us` | instante monotônico criado **aqui**, na origem |

A tripla (`source_id`, `boot_id`, `sequence`) é a identidade canônica da
mensagem lógica, e é ela — não o frame físico — que o resto do caminho
preserva. O timestamp é criado neste ponto e por ninguém mais: o gráfico do
outro lado é plotado por ele, não pela hora de chegada.

→ [Frame BTP v1](BTP_V1.md), [Convenções e glossário](CONVENTIONS.md)

## 3. Cifra do payload (opcional)

Se o canal estiver configurado com `ENCRYPTED`, é **agora** que a cifra
acontece — sobre o payload lógico inteiro, antes de qualquer fragmentação. O
payload passa a ser `ciphertext ‖ tag` e cresce exatamente 16 octetos por
mensagem (10 → **26 octetos** no exemplo), o `version` do envelope vira
`0x02`, e o nonce sai de campos que o header já carrega.

Com `ENCRYPTED` limpo, esta etapa não existe e o frame é byte a byte o que
sempre foi.

→ [Criptografia AEAD do payload](CRYPTO.md)

## 4. Fragmentação

Se o payload lógico não couber no limite do transporte, ele é cortado em
fatias consecutivas; cada fatia vira um frame BTP completo e independente,
com o mesmo `type`, `flags`, `timestamp_us`, `object_id` e a mesma tripla de
identidade, variando só `fragment_index` dentro de `fragment_count` (máximo
de 255 fragmentos).

No exemplo não há fragmentação: 26 octetos cabem folgados nos 210 de payload
do ESP-NOW.

→ [COBS, fragmentação e reassembly](STREAM_AND_REASSEMBLY.md)

## 5. CRC e serialização

Cada frame é serializado campo por campo — nunca copiando a memória de uma
`struct` — e recebe um CRC-32/ISO-HDLC próprio, calculado do primeiro octeto
de `magic` até o último do payload:

```text
frame = header (36) ‖ payload ‖ CRC32 (4)         frame_size = 40 + payload_size
```

No exemplo cifrado, `40 + 26 = 66 octetos`; em claro, `40 + 10 = 50`.

O CRC é por frame, não por mensagem: ele existe para descartar lixo de
transporte barato, antes de gastar ciclos em reassembly ou cripto. Ele detecta
corrupção acidental e **não** autentica nada.

→ [Frame BTP v1](BTP_V1.md), [Codec portátil](CODEC.md)

## 6. O salto ESP-NOW e o gateway

Cada datagrama ESP-NOW carrega exatamente um frame BTP, sem prefixo,
delimitador ou padding — o tamanho do datagrama é exatamente
`40 + payload_size`. O enlace é best effort: uma amostra perdida é uma
amostra perdida, sem ACK por amostra e sem retransmissão do dado.

O dongle valida o que chega — magic, versão, tamanhos, limite do transporte,
CRC e invariantes de fragmentação — e roteia por canal. O que ele
deliberadamente **não** faz: reescrever `source_id`, `boot_id`, `sequence` ou
`timestamp_us`, interpretar o payload binário como texto, ou redefinir o
schema. Ele é gateway, não tradutor.

→ [ESP-NOW](TRANSPORT_ESPNOW.md), [Arquitetura e domínios](ARCHITECTURE.md)

## 7. O framing do enlace USB

O mesmo frame, sem uma alteração de octeto no envelope, ganha do outro lado
do dongle o framing do enlace escolhido:

| | Serial (modo protocolado) | USB HID |
| --- | --- | --- |
| Unidade | `0x00 ‖ COBS(frame) ‖ 0x00` | relatório de 64 octetos: Report ID + prefixo de tamanho + 62 de frame |
| Frame máximo | 4096 octetos | 62 octetos |
| Payload máximo | 4056 octetos | 22 octetos |
| Modo console humano | sim, alternado por handshake | não existe: sempre protocolado |

As duas interfaces coexistem no mesmo dispositivo composto e permanecem
ativas ao mesmo tempo, cada uma com `source_id`, `boot_id` e sessão BTP
próprios. Um cliente escolhe uma por conexão; não há migração de sessão de
uma para a outra.

!!! note "Ponto em aberto"
    O teto de payload do USB HID (22 octetos) é menor que o do ESP-NOW (210).
    Uma mensagem que chegou do rádio próxima do teto não cabe em um relatório
    HID como está: ela teria que ser remontada e refragmentada pelo gateway, e
    nenhum capítulo de transporte descreve esse reenquadramento hoje. A
    criptografia já não é obstáculo — a
    [seção 8.3](BTP_V1.md#83-dados-associados-aad) foi escrita justamente para
    que o tag sobreviva a uma refragmentação sem o gateway ter a chave —, mas
    o comportamento no nível do transporte continua não especificado. Na
    prática, o caminho exercitado ponta a ponta é ESP-NOW → Serial/COBS.

→ [Serial (COBS)](TRANSPORT_SERIAL.md), [USB HID](TRANSPORT_USB_HID.md)

## 8. Decode no cliente

O cliente lê um stream, não mensagens. O decoder incremental acumula octetos
até fechar um bloco entre delimitadores, decodifica COBS e entrega um frame
candidato ao `decode()`, que valida na ordem: magic, versão, `header_size`,
tamanhos, limite do transporte, CRC e invariantes de fragmentação. Nenhum
campo é publicado ao chamador antes de tudo isso passar; um frame reprovado é
descartado sem NACK.

→ [Codec portátil](CODEC.md), [COBS, fragmentação e reassembly](STREAM_AND_REASSEMBLY.md)

## 9. Reassembly

Fragmentos são agrupados por (`source_id`, `boot_id`, `sequence`) e ordenados
por `fragment_index`. Duas fontes fragmentando ao mesmo tempo não se
confundem porque a chave inclui a identidade da origem — cenário que os
vetores canônicos cobrem explicitamente. Um reassembly incompleto é
descartado; não existe pedido de retransmissão de fragmento.

→ [COBS, fragmentação e reassembly](STREAM_AND_REASSEMBLY.md)

## 10. Verificação do tag

Só agora, com a mensagem lógica inteira remontada, o tag AEAD é verificado —
sobre `ciphertext ‖ tag` e tendo os 36 octetos do header como dados
associados. Tag inválido significa mensagem descartada antes de qualquer
entrega ao consumidor, pela mesma política do CRC divergente. Verificação por
fragmento não existe e não seria possível: o tag pertence à mensagem.

→ [Criptografia AEAD do payload](CRYPTO.md)

## 11. Schema e binding

Com o payload lógico em claro na mão, o cliente lê o `schema_version` dos
dois primeiros octetos, resolve a tripla (`source_id`, `topic_id`,
`schema_version`) contra o manifesto que descobriu do dongle e obtém a lista
ordenada de campos — nome, tipo, unidade, escala, offset. Só então os 8
octetos de corpo voltam a ser `counter` e `value`.

Um `schema_version` desconhecido não é adivinhado: sem schema, a amostra não
é decodificada.

→ [Telemetria, arrays e schemas](TELEMETRY.md),
[Comandos, manifesto, sessão e terminal](COMMANDS_AND_ACTIONS.md)

## 12. Apresentação

O gráfico usa `timestamp_us` da origem, não o instante de chegada — é o que
torna a série imune à latência do rádio, à fila do dongle e ao agendamento do
desktop. A apresentação pode ter rótulo, cor e layout locais; identidade,
unidade e semântica vêm do catálogo.

→ [Arquitetura e domínios](ARCHITECTURE.md)

## O caminho de volta: um comando

```text
TraceView                    bally_dongle                    bally_OS
1 escolhe a ação do manifesto
2 COMMAND_REQUEST, sequence = request_id
   ---------------------->   3 roteia (ou executa a ação persistida)
                               -------------------------->   4 deduplica
                                                            5 executa
                            <--------------------------     6 COMMAND_RESULT
   <----------------------                                    reply_to_sequence
7 correlaciona e mostra o estado
```

Três diferenças em relação à telemetria importam:

- **Há correlação.** O `sequence` do envelope é o `request_id`, e a resposta
  aponta de volta por `reply_to_sequence` mais a identidade do requisitante
  (`request_source_id`, `request_boot_id`). Amostras de telemetria não têm
  esse par pergunta/resposta porque não são operações.
- **Há deduplicação.** O executor precisa reconhecer uma requisição repetida
  e não executá-la duas vezes — a mesma tripla identifica a mesma
  requisição, e o resultado é reemitido em vez de a ação ser refeita.
- **O catálogo é do gateway.** A ação existe porque o manifesto do dongle a
  anunciou; o cliente envia parâmetros para algo descoberto, não para algo
  que ele definiu localmente.

→ [Comandos, manifesto, sessão e terminal](COMMANDS_AND_ACTIONS.md)

## Onde cada regra vive

| Etapa | Documento normativo |
| --- | --- |
| Amostra, schema, encodings, arrays | [TELEMETRY.md](TELEMETRY.md) |
| Envelope, identidade, CRC, limites, fragmentação | [BTP_V1.md](BTP_V1.md) |
| Cifra, nonce, AAD, tag, chave | [BTP_V1.md](BTP_V1.md) §8 e [CRYPTO.md](CRYPTO.md) |
| API do codec, ordem de validação | [CODEC.md](CODEC.md) |
| COBS, decoder incremental, reassembly | [STREAM_AND_REASSEMBLY.md](STREAM_AND_REASSEMBLY.md) |
| Datagramas, entrega, retry | [TRANSPORT_ESPNOW.md](TRANSPORT_ESPNOW.md) |
| Framing serial, sessão, posse da porta | [TRANSPORT_SERIAL.md](TRANSPORT_SERIAL.md) |
| Relatórios HID, dispositivo composto | [TRANSPORT_USB_HID.md](TRANSPORT_USB_HID.md) |
| Comandos, manifesto, assinatura, terminal | [COMMANDS_AND_ACTIONS.md](COMMANDS_AND_ACTIONS.md) |
| Prova de que sua implementação está certa | [CONFORMANCE.md](CONFORMANCE.md) |
