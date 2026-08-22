# ADR 0007: wire format do envelope `version == 0x01`

- Estado: Aceita
- Data: 2026-08-09
- Impacto SemVer: baseline do wire v1, anterior à primeira release publicada (`v1.1.0-beta`)

## Contexto

ESP-IDF, Arduino e Qt precisam produzir os mesmos bytes sem depender de ABI,
alinhamento ou representação de `struct`. O envelope também precisa preservar
origem, boot, sequência e timestamp durante o roteamento, suportar payload
binário e caber no datagrama ESP-NOW de 250 octetos.

## Decisão

O BTP usa o cabeçalho fixo de 36 octetos especificado em
[`../BTP_V1.md`](../BTP_V1.md), seguido do payload válido e de CRC-32/ISO-HDLC
little-endian. O frame não possui padding.

O limite ESP-NOW é 250 octetos, deixando 210 para payload. O frame BTP
decodificado na serial é limitado a 4096 octetos, deixando 4056 para payload.
Mensagens lógicas maiores usam fragmentação comum; todos os fragmentos
compartilham (`source_id`, `boot_id`, `sequence`).

`source_id` é estável e único, `boot_id` muda a cada boot, `sequence` é
atribuída por mensagem lógica e `timestamp_us` é monotônico, em microssegundos,
na origem. Gateways preservam esses campos.

Tipos e flags não atribuídos são rejeitados. Extensões não podem ser inferidas
ou ignoradas silenciosamente no v1.

## Consequências

- O tamanho de um frame é determinístico: `40 + payload_size`.
- Um frame ESP-NOW máximo ocupa exatamente os 250 octetos disponíveis.
- Payloads com qualquer valor de byte são transportados sem terminador.
- Reassembly pode separar mensagens pela identidade completa da origem.
- Buffers de recepção têm limites estáticos e frames grandes precisam ser
  fragmentados.
- Uma mudança futura de offset, largura, algoritmo de CRC, limite ou semântica
  exige atualização do contrato, ADR, vetores e classificação SemVer.

## Alternativas consideradas

- **Transmitir uma `struct`:** rejeitado por padding, alinhamento, ABI e
  endianness diferentes entre plataformas.
- **Completar sempre 250 octetos no ESP-NOW:** rejeitado por desperdiçar tempo
  de rádio e tornar ambígua a extensão válida do payload.
- **IDs de 64 bits em um cabeçalho maior:** rejeitado nesta versão para manter
  210 octetos de payload; a unicidade de `source_id` é responsabilidade do
  provisionamento.
- **Ignorar flags desconhecidas:** rejeitado porque uma flag pode mudar a
  interpretação ou as garantias do frame.

