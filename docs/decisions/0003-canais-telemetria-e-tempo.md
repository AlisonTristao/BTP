# ADR 0003: canais, telemetria e tempo

- Estado: Aceita
- Data: 2026-08-09

## Contexto

Eventos humanos, séries temporais, operações e sessões interativas têm
garantias e ritmos diferentes. Vincular telemetria à interface gráfica também
impediria outros consumidores e layouts.

## Decisão

- `LOG`, `TELEMETRY`, `COMMAND` e `TERMINAL` são canais lógicos distintos.
- O ID de telemetria identifica um tópico, nunca um gráfico.
- Uma visualização seleciona dados por `source + topic + field`.
- O timestamp é criado na origem e nunca substituído por um gateway.
- O canal de log é destinado a eventos e diagnósticos; amostras frequentes
  são publicadas pelo canal de telemetria, com schema próprio.

## Consequências

- Logs não são analisados como fonte de gráfico.
- Um consumidor pode criar várias visualizações do mesmo campo sem alterar o
  produtor.
- Roteamento e retransmissão preservam identidade e tempo da amostra.
- Schema, representação exata da identidade e unidade do timestamp ainda
  precisam ser definidos nos tópicos de wire e telemetria.
