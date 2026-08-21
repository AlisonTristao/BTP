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
| [0007 - Wire format do envelope BTP v1](0007-wire-format-btp-v1.md) | Aceita | Layout, identidade, CRC, fragmentação e limites do frame v1 |
| [0008 - Payloads e schemas de telemetria](0008-payloads-e-schemas-de-telemetria.md) | Aceita | Identidade de tópicos, encodings, tipos, arrays e binding de telemetria |
| [0009 - Comandos, descoberta, sessão e terminal](0009-comandos-descoberta-sessao-terminal.md) | Aceita | Correlação, deduplicação, manifesto, assinatura, status e terminal |
| [0010 - Perfis de transporte ESP-NOW e Serial/COBS](0010-perfis-de-transporte-espnow-serial.md) | Aceita | Datagramas, entrega, retry, COBS, ressincronização, SerialMux e capacidade |
| [0011 - Perfil de transporte USB HID](0011-perfil-de-transporte-usb-hid.md) | Aceita | Dispositivo composto CDC+HID, relatórios de 64 octetos, sessão sempre protocolada |
| [0012 - Criptografia AEAD do payload (v2.0)](0012-criptografia-aead-payload.md) | Proposta | Flag `ENCRYPTED`, AES-128-GCM, sub-campo `CIPHER_ID` no wire, nonce de `source_id`/`boot_id`/`sequence`, AAD do header, chave fora do wire |

## Como criar uma decisão

Um novo ADR deve registrar contexto, decisão, consequências e alternativas
relevantes. Mudanças que afetem o wire também seguem
[`CONTRIBUTING.md`](../CONTRIBUTING.md) e a
[política de versionamento](../VERSIONING.md).
