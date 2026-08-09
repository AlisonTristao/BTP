# ADR 0008: payloads e schemas de telemetria

- Estado: Aceita
- Data: 2026-08-09
- Impacto SemVer: baseline inicial destinada a `v1.0.0`; não altera release publicada

## Contexto

Produtores embarcados precisam emitir amostras frequentes com pouco overhead,
enquanto consumidores Qt precisam decodificá-las sem depender de ABI, nomes ou
configuração de gráficos. Arrays, ausência de medição e evolução de schemas
também precisam ter representação inequívoca.

## Decisão

Para `TELEMETRY`, `object_id` é o `topic_id`; o tópico é identificado por
(`source_id`, `topic_id`). Todo payload lógico começa com
`schema_version:uint16_le`, formando a identidade completa do schema.

`PACKED_LE` é o encoding padrão de produção. Seus campos são serializados em
ordem de schema, sem padding; arrays fixos omitem contagem, arrays variáveis
usam `element_count:uint16_le` e campos nullable usam um bitmap inicial.
Tipos inteiros têm largura fixa, floats usam bits IEEE-754 little-endian e
valores não finitos são rejeitados.

Schemas atribuem `field_id` estável e carregam tipo, ordem, unidade, escala,
offset, quantidade e nulabilidade fora da amostra. O binding do cliente usa
(`source_id`, `topic_id`, `field_id`), nunca identidade de gráfico. As regras
completas estão em [`../TELEMETRY.md`](../TELEMETRY.md).

## Consequências

- O corpo comum não repete nomes, separadores, unidades ou terminadores.
- Zeros e quaisquer outros octetos são preservados como dados.
- Um decoder precisa obter o schema exato antes de publicar valores.
- Uma mudança de interpretação exige nova versão e não admite fallback.
- Arrays variáveis são limitados pelo schema e podem ser validados antes da
  alocação.
- Amostras fragmentadas só são observáveis depois do reassembly completo.

## Alternativas consideradas

- **Repetir o schema em cada amostra:** rejeitado pelo overhead em ESP-NOW.
- **Usar nomes ou IDs de gráfico no payload:** rejeitado por acoplar dados à
  apresentação.
- **Transmitir `struct` C/C++ diretamente:** rejeitado por ABI, padding,
  alinhamento e endianness.
- **Usar NaN como ausência:** rejeitado porque `nullable` torna ausência
  explícita e interoperável.
- **Usar TLV em toda produção:** rejeitado pelo custo por campo; permanece
  disponível quando extensibilidade esparsa justificar o overhead.
