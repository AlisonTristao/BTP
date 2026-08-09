# ADR 0001: fonte canônica e ausência de legado

- Estado: Aceita
- Data: 2026-08-09

## Contexto

Três projetos precisam compartilhar o mesmo contrato. Cópias locais da
especificação ou do codec criariam divergência, e compatibilidade com o
protocolo anterior aumentaria permanentemente a ambiguidade e a superfície de
testes.

## Decisão

`bally_protocol` é a única fonte da especificação, implementação compartilhada
e vetores de conformidade. Consumidores fixam uma versão ou revisão imutável e
não mantêm cópias independentes desses artefatos.

Não haverá suporte ao protocolo legado. A migração pode remover a `struct
message`, parsers seriais e demais caminhos antigos; nenhum adaptador, flag,
fallback ou autodetecção será adicionado.

## Consequências

- Mudanças começam neste repositório e chegam aos consumidores por atualização
  explícita de dependência.
- A migração precisa ser coordenada, pois não existirá operação híbrida.
- Peers incompatíveis falham de forma visível em vez de tentar interpretar
  bytes como um protocolo alternativo.
