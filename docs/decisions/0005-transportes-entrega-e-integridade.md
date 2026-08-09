# ADR 0005: transportes, entrega e integridade

- Estado: Aceita
- Data: 2026-08-09

## Contexto

ESP-NOW transporta pacotes, enquanto a USB Serial é um fluxo de bytes. Canais
também exigem políticas distintas de confiabilidade e interação humana.

## Decisão

- A serial protocolada usa frames BTP delimitados por COBS.
- O console humano permanece disponível como modo separado.
- Comandos carregam request ID, produzem resultado e suportam deduplicação.
- Telemetria é best effort e não recebe ACK por amostra.
- CRC detecta corrupção acidental, mas não fornece autenticação.

## Consequências

- O modo protocolado não mistura linhas de console com frames BTP.
- Reenvio de comando não deve repetir efeitos já concluídos.
- Perdas de telemetria são tratadas por métricas, sequência e atualização
  futura, não por confirmação de cada amostra.
- Autenticidade ou confidencialidade, se necessárias, exigirão mecanismo de
  segurança próprio além do CRC.
- Polinômio e cobertura do CRC, regras COBS, sessão, fragmentação e reassembly
  ainda serão especificados.
