# Registro de decisões de arquitetura

As decisões usam registros imutáveis: quando uma decisão aceita mudar, um novo
registro a substitui e aponta para o anterior. O histórico não é reescrito.

Estados permitidos: `Proposta`, `Aceita`, `Substituída` e `Rejeitada`.

## Índice

| ADR | Estado | Decisões cobertas do plano geral |
| --- | --- | --- |
| [0001 - Fonte canônica e ausência de legado](0001-fonte-canonica-e-sem-legado.md) | Aceita | 1 e regra de não copiar contrato/código |
| [0002 - Modelo binário e serialização](0002-modelo-binario-e-serializacao.md) | Aceita | 2 a 7 |
| [0003 - Canais, telemetria e tempo](0003-canais-telemetria-e-tempo.md) | Aceita | 8 a 12 |
| [0004 - Gateway, catálogo e apresentação](0004-gateway-catalogo-e-apresentacao.md) | Aceita | 13 e responsabilidades de dongle/TraceView |
| [0005 - Transportes, entrega e integridade](0005-transportes-entrega-e-integridade.md) | Aceita | 14 a 18 |
| [0006 - Versionamento conjunto](0006-versionamento-conjunto.md) | Aceita | Política de versão do protocolo e biblioteca |

## Como criar uma decisão

Um novo ADR deve registrar contexto, decisão, consequências e alternativas
relevantes. Mudanças que afetem o wire também seguem
[`CONTRIBUTING.md`](../../CONTRIBUTING.md) e a
[política de versionamento](../VERSIONING.md).
