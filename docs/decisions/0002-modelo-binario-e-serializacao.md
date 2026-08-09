# ADR 0002: modelo binário e serialização

- Estado: Aceita
- Data: 2026-08-09

## Contexto

ESP32, Arduino e aplicações Qt podem usar compiladores, ABIs, alinhamento e
endianness diferentes. O protocolo também precisa transportar todos os valores
de byte sem depender de strings.

## Decisão

- Nenhuma `struct` C/C++ é transmitida por `reinterpret_cast` ou cópia de sua
  memória.
- O wire usa tipos de largura fixa e serialização little-endian explícita.
- Todo payload é uma sequência opaca de bytes com tamanho explícito.
- `0x00`, CR, LF e qualquer outro byte são dados válidos, nunca terminadores
  implícitos do payload.
- Telemetria de produção usa `PACKED_LE`; CSV existe apenas como opção de teste
  e diagnóstico.

## Consequências

- Encoder e decoder precisam ler e escrever cada campo deliberadamente.
- Vetores binários serão idênticos entre plataformas.
- Framing de transporte deve respeitar tamanho e pode envolver COBS; não pode
  procurar fim de linha dentro do payload.
- Offsets, tipos e layout de `PACKED_LE` permanecem para os ADRs e
  especificações dos próximos tópicos.
