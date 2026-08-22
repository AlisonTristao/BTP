# ADR 0006: versionamento conjunto do contrato

- Estado: Aceita
- Data: 2026-08-09

## Contexto

Especificação, biblioteca e vetores incompatíveis entre si inviabilizam a
conformidade. Consumidores também precisam identificar precisamente o contrato
que implementam.

## Decisão

O repositório usa uma linha de releases SemVer `vMAJOR.MINOR.PATCH` para a
especificação, a biblioteca compartilhada e os vetores correspondentes. O wire
format inicial congelado é o wire v1; enquanto o contrato estiver em elaboração,
as releases carregam sufixo `-beta`.

Mudança incompatível incrementa `MAJOR`; extensão compatível incrementa
`MINOR`; correção sem mudança de bytes ou semântica incrementa `PATCH`. A
política completa está em [`../VERSIONING.md`](../VERSIONING.md).

## Consequências

- Cada consumidor fixa uma tag ou revisão imutável.
- Toda mudança de wire inclui classificação de versão e vetores de teste.
- A biblioteca expõe sua versão e o intervalo de protocolo suportado.
- Releases da biblioteca e do contrato não podem divergir silenciosamente.
